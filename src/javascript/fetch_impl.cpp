#include "fetch_impl.hpp"

#include "errors/errors.hpp"
#include "intlimit/intlimit.hpp"
#include "javascript.hpp"
#include "owned/quickjs.hpp"
#include "owned/uv.hpp"
#include "void_ptr/void_ptr.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/header.h>
#include <curl/multi.h>
#include <curl/system.h>
#include <curl/urlapi.h>
#include <quickjs.h>
#include <uv.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace errors = amj::errors;
namespace intlimit = amj::intlimit;
namespace owned = amj::owned;
namespace this_thread = std::this_thread;
namespace void_ptr = amj::void_ptr;

namespace {

constexpr std::size_t bodyBufferLimit = 256U << 10U /* 256 KiB */;
constexpr std::size_t requestBodyLimit = 32U << 20U /* 32 MiB */;

// Retain native codes without allocating diagnostic strings in callbacks.
class UvErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "libuv";
    }
    std::string message(int code) const override {
        return uv_strerror(code);
    }
};

const UvErrorCategory uvErrorCategory; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

class CurlMultiErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "curl.multi";
    }
    std::string message(int code) const override {
        return curl_multi_strerror(static_cast<CURLMcode>(code));
    }
};

const CurlMultiErrorCategory curlMultiErrorCategory; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

// Passed as CURLOPT_HEADERFUNCTION to read the response headers.
// NOLINTNEXTLINE(misc-const-correctness): curl_write_callback requires char*.
std::size_t curlHeaderFunction(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    FetchState* fetchState = static_cast<void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
    if (intlimit::multiplyOverflows(size, nitems)) {
        return CURL_WRITEFUNC_ERROR;
    }
    const std::size_t realsize = size * nitems;
    const std::string_view line(buffer, realsize);

    // If responseHeadersComplete, we are handling response trailers.
    if (fetchState->responseHeadersComplete) {
        if (intlimit::addOverflows(fetchState->responseTrailersSize, realsize)) {
            return CURL_WRITEFUNC_ERROR;
        }
        if (fetchState->responseTrailersSize + realsize > (4U << 10U /* 4 KiB */)) {
            fetchState->error = errors::message("Response trailers exceed 4 KiB");
            return CURL_WRITEFUNC_ERROR;
        }
        fetchState->responseTrailersSize += realsize;
        return realsize;
    }

    // Every fresh response following a redirect gets a new responseHeadersSize
    // budget.
    if (line.starts_with("HTTP/") && fetchState->responseRedirecting) {
        fetchState->responseHeadersSize = 0;
    }
    if (intlimit::addOverflows(fetchState->responseHeadersSize, realsize)) {
        return CURL_WRITEFUNC_ERROR;
    }
    if (fetchState->responseHeadersSize + realsize > (10U << 20U /* 10 MiB */)) {
        fetchState->error = errors::message("Response headers exceed 10 MiB");
        return CURL_WRITEFUNC_ERROR;
    }
    fetchState->responseHeadersSize += realsize;

    // If it's a fresh response, reset the responseHeaders and the
    // responseRedirecting flag.
    if (line.starts_with("HTTP/")) {
        fetchState->responseHeaders.clear();
        fetchState->responseRedirecting = false;
        return realsize;
    }

    // Response headers have not yet completed.
    if (line != "\r\n" && line != "\n") {
        return realsize;
    }

    // If we reach here, response headers have completed. Get the responseCode.
    long responseCode = 0;
    CURLcode curlCode = curl_easy_getinfo(fetchState->ownedCurlEasyHandle, CURLINFO_RESPONSE_CODE, &responseCode);
    if (curlCode != CURLE_OK) {
        fetchState->error = errors::message(curl_easy_strerror(curlCode));
        return CURL_WRITEFUNC_ERROR;
    }

    // If it's informational 1XX, ignore the responseCode.
    if (responseCode >= 100 && responseCode < 200) {
        if (responseCode == 101) {
            fetchState->error = errors::message("HTTP protocol upgrades are not supported");
            return CURL_WRITEFUNC_ERROR;
        }
        return realsize;
    }

    // If it's a redirect 3XX and the Location header is present, ignore the
    // responseCode and set the responseRedirecting flag.
    if (responseCode == 301 || responseCode == 302 || responseCode == 303 || responseCode == 307 || responseCode == 308) {
        curl_header* curlHeader = nullptr;
        const CURLHcode curlHCode = curl_easy_header(fetchState->ownedCurlEasyHandle, "location", 0, CURLH_HEADER, -1, &curlHeader);
        if (curlHCode != CURLHE_OK) {
            if (curlHCode != CURLHE_MISSING && curlHCode != CURLHE_NOHEADERS) {
                fetchState->error = errors::message("Could not obtain response redirect header");
                return CURL_WRITEFUNC_ERROR;
            }
        }
        if (curlHeader != nullptr && curlHeader->value[0] != '\0') {
            fetchState->responseRedirecting = true;
            return realsize;
        }
    }

    fetchState->responseCode = responseCode;

    // Get the effectiveURL.
    char* effectiveURL = nullptr; // NOLINT(misc-const-correctness): CURLINFO_EFFECTIVE_URL requires char**.
    curlCode = curl_easy_getinfo(fetchState->ownedCurlEasyHandle, CURLINFO_EFFECTIVE_URL, &effectiveURL);
    if (curlCode != CURLE_OK) {
        fetchState->error = errors::message("Could not obtain response URL");
        return CURL_WRITEFUNC_ERROR;
    }
    if (effectiveURL != nullptr) {
        fetchState->effectiveURL = effectiveURL;
    }

    // Get the responseHeaders.
    curl_header* curlHeader = nullptr;
    while (true) {
        curlHeader = curl_easy_nextheader(fetchState->ownedCurlEasyHandle, CURLH_HEADER, -1, curlHeader);
        if (curlHeader == nullptr) {
            break;
        }
        std::string name(curlHeader->name);
        const std::string value(curlHeader->value);
        for (char& ch : name) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        fetchState->responseHeaders.push_back({name, value});
    }
    fetchState->responseHeadersComplete = true;
    return realsize;
}

// Passed as CURLOPT_WRITEFUNCTION to read the response body.
std::size_t curlWriteFunction(char* ptr, std::size_t size, std::size_t nmemb, const void* userdata) {
    FetchState* fetchState = static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
    if (intlimit::multiplyOverflows(size, nmemb)) {
        return CURL_WRITEFUNC_ERROR;
    }
    const std::size_t realsize = size * nmemb;

    // Body is empty.
    if (realsize == 0) {
        return 0;
    }

    // Ignore the body if we are redirecting.
    if (fetchState->responseRedirecting) {
        return realsize;
    }

    // If the responseBufferSize exceeds the bodyBufferLimit, pause curl
    // transfers.
    if (intlimit::addOverflows(fetchState->responseBufferSize, realsize)) {
        return CURL_WRITEFUNC_ERROR;
    }
    if (fetchState->responseBufferSize + realsize > bodyBufferLimit) {
        fetchState->responseCurlTransferStatus = CurlTransferStatus::Paused;
        return CURL_WRITEFUNC_PAUSE;
    }

    // Else append the bytes to the responseBuffer.
    fetchState->responseBuffer.push_back(std::vector<uint8_t>(ptr, ptr + realsize));
    fetchState->responseBufferSize += realsize;
    return realsize;
}

// Drive curl transfers, then mark the ones that have completed. Returns true
// on success, or false if a fatal error is recorded in pendingError.
[[nodiscard]] bool driveCurlTransfers(FetchClient& fetchClient, curl_socket_t socket, int eventBitmask) {
    // Check for pendingErrors before we call curl_multi_socket_action().
    if (fetchClient.pendingError->hasValue()) {
        return false;
    }

    // Tell curl to read and write socket data.
    int runningHandles = 0;
    const CURLMcode curlMCode = curl_multi_socket_action(fetchClient.ownedCurlMultiHandle, socket, eventBitmask, &runningHandles);
    if (curlMCode != CURLM_OK) {
        if (!fetchClient.pendingError->hasValue()) {
            fetchClient.pendingError->errorCode = std::error_code(static_cast<int>(curlMCode), curlMultiErrorCategory);
        }
        return false;
    }

    // curl_multi_socket_action() may invoke curlSocketFunction or
    // curlTimerFunction, which may have set pendingError. Check before
    // proceeding.
    if (fetchClient.pendingError->hasValue()) {
        return false;
    }

    // Mark the fetchStates that have completed.
    while (true) {
        int msgsInQueue = 0;
        const CURLMsg* curlMsg = curl_multi_info_read(fetchClient.ownedCurlMultiHandle, &msgsInQueue);
        if (curlMsg == nullptr) {
            break;
        }
        if (curlMsg->msg != CURLMSG_DONE) {
            continue;
        }
        if (!fetchClient.fetchStateMap.contains(curlMsg->easy_handle)) {
            std::terminate(); // A completed transfer must have registered callback storage.
        }
        FetchState& fetchState = fetchClient.fetchStateMap.at(curlMsg->easy_handle);
        fetchState.responseCurlTransferStatus = CurlTransferStatus::Completed;
        if (curlMsg->data.result != CURLE_OK || !fetchState.responseHeadersComplete) {
            fetchState.error.errorCode = errors::ErrorCode::GenericError;
            std::string errorMessage;
            if (fetchState.error.errorMessage != nullptr) {
                errorMessage = *fetchState.error.errorMessage;
            }
            if (curlMsg->data.result != CURLE_OK) {
                if (!errorMessage.empty()) {
                    errorMessage.append(": ");
                }
                errorMessage.append(curl_easy_strerror(curlMsg->data.result));
            }
            if (!fetchState.responseHeadersComplete) {
                if (!errorMessage.empty()) {
                    errorMessage.append(": ");
                }
                errorMessage.append("HTTP response ended without final headers");
            }
            fetchState.error.errorMessage = std::make_shared<const std::string>(std::move(errorMessage));
        }
    }

    // Signal the uv async handle, which triggers settlePendingPromises().
    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle.get()));
    const int status = uv_async_send(fetchClient.uvAsyncHandle.get());
    if (status != 0) {
        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle.get()));
        // Nothing since the post-curl checkpoint can invoke JS/curl callbacks;
        // this is the first error, so no second mailbox check is needed.
        fetchClient.pendingError->errorCode = std::error_code(status, uvErrorCategory);
        return false;
    }
    return true;
}

// curl calls this function to specify which socket events should trigger
// driveCurlTransfers().
//
// 1. CURL_POLL_NONE: Stop scheduling driveCurlTransfers() for socket events,
// retaining the poll handle.
//
// 2. CURL_POLL_IN: Call driveCurlTransfers() when this socket becomes readable.
//
// 3. CURL_POLL_OUT: Call driveCurlTransfers() when this socket becomes writable.
//
// 4. CURL_POLL_INOUT: Call driveCurlTransfers() when this socket becomes
// readable or writable.
//
// 5. CURL_POLL_REMOVE: Stop scheduling driveCurlTransfers() for socket events
// and close the poll handle.
int curlSocketFunction(CURL* /* curlEasyHandle */, curl_socket_t socket, int what, const void* clientp, void* /* socketp */) {
    struct PollData {
        FetchClient* fetchClient = nullptr;
        curl_socket_t socket = CURL_SOCKET_BAD;
    };
    FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(clientp)->cast<FetchClient*>();

    // Stop watching this socket and close its poll handle.
    if (what == CURL_POLL_REMOVE) {
        fetchClient->uvPollHandleMap.erase(socket);
        return 0;
    }

    // Get the uvPollHandle associated with this socket, creating it if not
    // exists.
    uv_poll_t* uvPollHandle = nullptr;
    if (fetchClient->uvPollHandleMap.contains(socket)) {
        uvPollHandle = fetchClient->uvPollHandleMap.at(socket).get();
    } else {
        owned::UvHandle<uv_poll_t> ownedUvPollHandle(std::make_unique<uv_poll_t>());
        const int status = uv_poll_init_socket(fetchClient->uvLoop, ownedUvPollHandle.get(), socket);
        if (status != 0) {
            if (!fetchClient->pendingError->hasValue()) {
                fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
            }
            return -1;
        }
        ownedUvPollHandle.setInitialized();
        ownedUvPollHandle.get()->data = new void_ptr::VoidPtr(
            std::make_unique<PollData>(PollData{
                .fetchClient = fetchClient,
                .socket = socket,
            })
        );
        fetchClient->uvPollHandleMap.insert_or_assign(socket, std::move(ownedUvPollHandle));
        uvPollHandle = fetchClient->uvPollHandleMap.at(socket).get();
    }

    // Stop watching this socket, retaining its poll handle.
    if (what == CURL_POLL_NONE) {
        const int status = uv_poll_stop(uvPollHandle);
        if (status != 0) {
            if (!fetchClient->pendingError->hasValue()) {
                fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
            }
            return -1;
        }
        return 0;
    }

    // libuv calls uvPollCallback when the socket is readable, writable, or
    // encounters an error.
    const auto uvPollCallback = [](uv_poll_t* uvPollHandle, int status, int events) noexcept {
        const PollData& pollData = static_cast<void_ptr::VoidPtr*>(uvPollHandle->data)->cast<PollData>();
        FetchClient* fetchClient = pollData.fetchClient;
        const curl_socket_t socket = pollData.socket;
        try {
            unsigned int eventBitmask = 0;
            if (status < 0) {
                eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_ERR);
            }
            if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_READABLE)) != 0) {
                eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_IN);
            }
            if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_WRITABLE)) != 0) {
                eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_OUT);
            }
            const bool ok = driveCurlTransfers(*fetchClient, socket, static_cast<int>(eventBitmask));
            if (!ok) {
                return;
            }
        } catch (...) {
            if (!fetchClient->pendingError->hasValue()) {
                fetchClient->pendingError->exceptionPtr = std::current_exception();
            }
        }
    };

    unsigned int events = 0;
    if (what == CURL_POLL_IN) {
        // Call driveCurlTransfers() when this socket becomes readable.
        events = static_cast<unsigned int>(UV_READABLE);
    } else if (what == CURL_POLL_OUT) {
        // Call driveCurlTransfers() when this socket becomes writable.
        events = static_cast<unsigned int>(UV_WRITABLE);
    } else if (what == CURL_POLL_INOUT) {
        // Call driveCurlTransfers() when this socket becomes readable or
        // writable.
        events = static_cast<unsigned int>(UV_READABLE) | static_cast<unsigned int>(UV_WRITABLE);
    }
    const int status = uv_poll_start(uvPollHandle, static_cast<int>(events), uvPollCallback);
    if (status != 0) {
        if (!fetchClient->pendingError->hasValue()) {
            fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
        }
        return -1;
    }
    return 0;
}

// curl calls this function to update its timeout requirements.
//
// 1. timeout_ms < 0: Cancel the scheduled call to driveCurlTransfers().
//
// 2. timeout_ms >= 0: Schedule driveCurlTransfers() to run after this delay.
int curlTimerFunction(CURLM* /* curlMultiHandle */, long timeout_ms, const void* clientp) {
    const FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(clientp)->cast<FetchClient*>();

    // Cancel the scheduled call to driveCurlTransfers().
    if (timeout_ms < 0) {
        const int status = uv_timer_stop(fetchClient->uvTimerHandle.get());
        if (status != 0) {
            if (!fetchClient->pendingError->hasValue()) {
                fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
            }
            return -1;
        }
        return 0;
    }

    // Refresh libuv's cached time so the delay starts from now.
    uv_update_time(fetchClient->uvLoop);

    // libuv calls uvTimerCallback when the timer expires, which calls
    // driveCurlTransfers().
    const auto uvTimerCallback = [](uv_timer_t* uvTimerHandle) noexcept {
        FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(uvTimerHandle->data)->cast<FetchClient*>();
        try {
            const bool ok = driveCurlTransfers(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
            if (!ok) {
                return;
            }
        } catch (...) {
            if (!fetchClient->pendingError->hasValue()) {
                fetchClient->pendingError->exceptionPtr = std::current_exception();
            }
        }
    };

    // Schedule driveCurlTransfers() to run after this delay.
    const int status = uv_timer_start(fetchClient->uvTimerHandle.get(), uvTimerCallback, static_cast<uint64_t>(timeout_ms), 0);
    if (status != 0) {
        if (!fetchClient->pendingError->hasValue()) {
            fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
        }
        return -1;
    }
    return 0;
}

// Settle pending promises with response readiness, body chunks, end-of-stream,
// or errors.
void settlePendingPromises(FetchClient* fetchClient) {
    // Snapshot the fetchStateMap's CURL* keys + fetchStateID into an entries
    // list before looping over the entries. The fetchStateMap can be modified
    // during iteration, so we need a stable list to iterate over.
    std::vector<std::pair<CURL*, uint64_t>> entries;
    entries.reserve(fetchClient->fetchStateMap.size());
    for (const std::pair<CURL* const, FetchState>& entry : fetchClient->fetchStateMap) {
        entries.push_back({entry.first, entry.second.fetchStateID});
    }

    // Loop over the entries.
    for (const std::pair<CURL*, uint64_t>& entry : entries) {
        if (fetchClient->pendingError->hasValue()) {
            return;
        }
        CURL* curlEasyHandle = entry.first;
        const uint64_t fetchStateID = entry.second;

        // Earlier iterations run JS promise jobs, which may close other
        // responses and remove their states from the map. Skip handles whose
        // states have been removed since entries was snapshotted.
        if (!fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            continue;
        }

        // Get the fetchState for the give curlEasyHandle.
        //
        // Curl may reuse CURL* pointers, so we need to additionally check if
        // the fetchStateID aligns.
        FetchState& fetchState = fetchClient->fetchStateMap.at(curlEasyHandle);
        if (fetchState.fetchStateID != fetchStateID) {
            continue;
        }

        // If fetchState was abandoned (the QuickJS GC finalizer sets this
        // flag), it means the user forgot to call fetchResponse.close() and is
        // no longer referencing the `fetchResponse` variable. Terminate its
        // current ready/read promises with AbortError.
        if (fetchState.abandoned) {
            const std::map<CURL*, FetchState>::node_type mapNode = fetchClient->fetchStateMap.extract(curlEasyHandle);
            if (mapNode.empty()) {
                std::terminate(); // An extracted abandoned state must exist.
            }
            FetchState& fetchState = mapNode.mapped();
            const errors::Error error = fetchState.close();
            if (error.hasValue()) {
                if (!fetchClient->pendingError->hasValue()) {
                    *fetchClient->pendingError = error;
                }
            }
            if (fetchClient->pendingError->hasValue()) {
                return;
            }
            // Retain settlement functions locally: close() releases the state's
            // JS references. These moves cannot throw, so close precedes all exits.
            owned::JSPromise responseReadyPromise = std::move(fetchState.responseReadyPromise);
            std::optional<owned::JSPromise> responseReadPromise = std::move(fetchState.responseReadPromise);
            const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(fetchClient->jsContext, responseReadyPromise.promise.get());
            if (responseReadyPromiseState != JS_PROMISE_PENDING && !responseReadPromise.has_value()) {
                continue;
            }
            owned::JSValue jsError = jsNewError(fetchClient->jsContext, "AbortError", "FetchResponse is closed");
            // Reject responseReadyPromise.
            if (responseReadyPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(fetchClient->jsContext, responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
                JS_FreeValue(fetchClient->jsContext, result);
            }
            // Reject responseReadPromise.
            if (responseReadPromise.has_value()) {
                const JSPromiseStateEnum responseReadPromiseState = JS_PromiseState(fetchClient->jsContext, responseReadPromise.value().promise.get());
                if (responseReadPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
                    JS_FreeValue(fetchClient->jsContext, result);
                }
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // If responseReadyPromise has not yet been settled, settle it first.
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED && responseReadyPromiseState != JS_PROMISE_REJECTED) {
            if (!fetchState.responseHeadersComplete) {
                // The transfer has not yet completed, don't settle the
                // responseReadyPromise.
                if (fetchState.responseCurlTransferStatus != CurlTransferStatus::Completed) {
                    continue;
                }

                // The transfer completed without receiving any response
                // headers. Reject responseReadyPromise.
                owned::JSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.error.errorMessage != nullptr ? *fetchState.error.errorMessage : std::string());
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
                fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
                continue;
            }

            // Resolve resonseReadyPromise.
            const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.resolveFunction.get(), JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // No responseReadPromise needs settling.
        if (!fetchState.responseReadPromise.has_value()) {
            continue;
        }

        // No error, buffered data, or end-of-stream to settle the
        // responseReadPromise with.
        if (!fetchState.error.hasValue() && fetchState.responseBuffer.empty() && fetchState.responseCurlTransferStatus != CurlTransferStatus::Completed) {
            continue;
        }

        // Move fetchState.responseReadPromise out to a local variable and
        // reset it so JS can request another read.
        owned::JSPromise responseReadPromise = std::move(fetchState.responseReadPromise.value());
        fetchState.responseReadPromise.reset();

        // Reject the responseReadPromise if the transfer encountered an error.
        if (fetchState.error.hasValue()) {
            owned::JSValue jsError = jsNewError(fetchClient->jsContext, "TypeError", fetchState.error.errorMessage != nullptr ? *fetchState.error.errorMessage : std::string());
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // No error or buffered chunks remain, and the transfer has completed.
        // Resolve the responseReadPromise with null to indicate end-of-stream.
        if (fetchState.responseBuffer.empty()) {
            JSValue jsNull = JS_NULL;
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &jsNull);
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // Pop the oldest chunk from fetchState.responseBuffer.
        const std::vector<uint8_t> chunk = std::move(fetchState.responseBuffer.front());
        fetchState.responseBuffer.pop_front();
        fetchState.responseBufferSize -= chunk.size();

        // If curl is currently paused and popping that chunk from the
        // responseBuffer dropped responseBufferSize to less than half of
        // bodyBufferLimit, unpause curl.
        if (fetchState.responseCurlTransferStatus == CurlTransferStatus::Paused && fetchState.responseBufferSize < bodyBufferLimit / 2) {
            fetchState.responseCurlTransferStatus = CurlTransferStatus::Running;
            const CURLcode curlCode = curl_easy_pause(curlEasyHandle, CURLPAUSE_CONT);
            if (curlCode != CURLE_OK) {
                fetchState.error = errors::message(curl_easy_strerror(curlCode));
                fetchState.responseCurlTransferStatus = CurlTransferStatus::Completed;
                if (fetchState.curlMultiHandle != nullptr) {
                    const CURLMcode curlMCode = curl_multi_remove_handle(fetchState.curlMultiHandle, curlEasyHandle);
                    if (curlMCode != CURLM_OK) {
                        if (!fetchClient->pendingError->hasValue()) {
                            fetchClient->pendingError->errorCode = std::error_code(static_cast<int>(curlMCode), curlMultiErrorCategory);
                        }
                        return;
                    }
                    fetchState.curlMultiHandle = nullptr;
                }
            }
            const bool ok = driveCurlTransfers(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
            if (!ok) {
                return;
            }
        }

        // Convert the chunk into a JS Uint8Array.
        owned::JSValue uint8Array(fetchClient->jsContext, JS_NewUint8ArrayCopy(fetchClient->jsContext, chunk.data(), chunk.size()));
        if (JS_IsException(uint8Array.get())) {
            owned::JSValue error(fetchClient->jsContext, JS_GetException(fetchClient->jsContext));
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // Resolve the responseReadPromise with the Uint8Array chunk.
        const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &uint8Array.get());
        JS_FreeValue(fetchClient->jsContext, result);
        fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
    }
}

std::mutex fetchResponseClassIDMutex = {};
std::map<JSRuntime*, JSClassID> fetchResponseClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

// Validate the receiver without requiring an open or ready response.
// The returned pointer is borrowed; failures leave a pending JS exception.
std::expected<FetchResponse*, JSValue> getFetchResponse(JSContext* jsContext, JSValueConst thisValue) {
    JSClassID fetchResponseClassID = 0;
    {
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
        if (!fetchResponseClassIDMap.contains(jsRuntime)) {
            return std::unexpected(JS_ThrowInternalError(jsContext, "FetchResponse class is not registered"));
        }
        fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
    }
    const void* opaque = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
    if (opaque == nullptr) {
        return std::unexpected(JS_EXCEPTION);
    }
    FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(opaque)->cast<FetchResponse>();
    if (this_thread::get_id() != fetchResponse.fetchClient->ownerThreadID) {
        std::terminate();
    }
    // JS can catch a native failure before control returns to the host.
    if (fetchResponse.fetchClient->pendingError->hasValue()) {
        return std::unexpected(JS_ThrowInternalError(jsContext, "FetchResponse: native failure"));
    }
    return &fetchResponse;
}

} // namespace

JSValue FetchResponse::read(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        std::expected<FetchResponse*, JSValue> fetchResponseResult = getFetchResponse(jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return fetchResponseResult.error();
        }
        const FetchResponse* fetchResponse = fetchResponseResult.value();
        if (fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.read: response is closed");
        }
        if (!fetchResponse->fetchClient->fetchStateMap.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.read: response state is missing");
        }
        FetchState& fetchState = fetchResponse->fetchClient->fetchStateMap.at(fetchResponse->curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED && responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.read: response not ready");
        }
        if (fetchState.responseReadPromise.has_value()) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.read: another read is already pending");
        }
        std::array resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        owned::JSValue promise(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(promise.get())) {
            return JS_EXCEPTION;
        }
        fetchState.responseReadPromise = owned::JSPromise{
            .promise = std::move(promise),
            .resolveFunction = owned::JSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = owned::JSValue(jsContext, resolvingFunctions.at(1)),
        };
        uv_ref(reinterpret_cast<uv_handle_t*>(fetchResponse->fetchClient->uvAsyncHandle.get()));
        const int status = uv_async_send(fetchResponse->fetchClient->uvAsyncHandle.get());
        if (status != 0) {
            uv_unref(reinterpret_cast<uv_handle_t*>(fetchResponse->fetchClient->uvAsyncHandle.get()));
            if (!fetchResponse->fetchClient->pendingError->hasValue()) {
                fetchResponse->fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
            }
            return JS_ThrowInternalError(jsContext, "FetchResponse.read: uv_async_send: %s", uv_strerror(status));
        }
        return JS_DupValue(jsContext, fetchState.responseReadPromise.value().promise.get());
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: native failure");
    }
}

JSValue FetchResponse::close(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        std::expected<FetchResponse*, JSValue> fetchResponseResult = getFetchResponse(jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return fetchResponseResult.error();
        }
        FetchResponse* fetchResponse = fetchResponseResult.value();
        CURL* curlEasyHandle = fetchResponse->curlEasyHandle;
        fetchResponse->curlEasyHandle = nullptr;
        if (curlEasyHandle == nullptr) {
            return JS_UNDEFINED;
        }
        const std::map<CURL*, FetchState>::node_type mapNode = fetchResponse->fetchClient->fetchStateMap.extract(curlEasyHandle);
        if (mapNode.empty()) {
            return JS_UNDEFINED;
        }
        FetchState& fetchState = mapNode.mapped();
        // Move the responseReadyPromise and responseReadPromise out of the
        // fetchState before calling close() on it.
        owned::JSPromise responseReadyPromise = std::move(fetchState.responseReadyPromise);
        std::optional<owned::JSPromise> responseReadPromise = std::move(fetchState.responseReadPromise);
        const errors::Error error = fetchState.close();
        if (error.hasValue()) {
            if (!fetchResponse->fetchClient->pendingError->hasValue()) {
                *fetchResponse->fetchClient->pendingError = error;
            }
        }
        if (fetchResponse->fetchClient->pendingError->hasValue()) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.close: native failure");
        }
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(jsContext, responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_PENDING && !responseReadPromise.has_value()) {
            return JS_UNDEFINED;
        }
        owned::JSValue jsError = jsNewError(jsContext, "AbortError", "FetchResponse is closed");
        if (responseReadyPromiseState == JS_PROMISE_PENDING) {
            const JSValue result = JS_Call(jsContext, responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
            JS_FreeValue(jsContext, result);
        }
        if (responseReadPromise.has_value()) {
            const JSPromiseStateEnum responseReadPromiseState = JS_PromiseState(jsContext, responseReadPromise.value().promise.get());
            if (responseReadPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(jsContext, responseReadPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
                JS_FreeValue(jsContext, result);
            }
        }
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: native failure");
    }
}

JSValue FetchResponse::getStatus(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        std::expected<FetchResponse*, JSValue> fetchResponseResult = getFetchResponse(jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return fetchResponseResult.error();
        }
        const FetchResponse* fetchResponse = fetchResponseResult.value();
        if (fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getStatus: response is closed");
        }
        if (!fetchResponse->fetchClient->fetchStateMap.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.getStatus: response state is missing");
        }
        const FetchState& fetchState = fetchResponse->fetchClient->fetchStateMap.at(fetchResponse->curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED && responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getStatus: response not ready");
        }
        return JS_NewInt64(jsContext, fetchState.responseCode);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.status: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.status: unexpected native failure");
    }
}

JSValue FetchResponse::getHeaders(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        std::expected<FetchResponse*, JSValue> fetchResponseResult = getFetchResponse(jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return fetchResponseResult.error();
        }
        const FetchResponse* fetchResponse = fetchResponseResult.value();
        const FetchClient* fetchClient = fetchResponse->fetchClient;
        if (fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getHeaders: response is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.getHeaders: response state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse->curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED && responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getHeaders: response not ready");
        }
        std::vector<JSONValue> headers;
        headers.reserve(fetchState.responseHeaders.size());
        for (const std::pair<std::string, std::string>& header : fetchState.responseHeaders) {
            headers.push_back(std::vector<JSONValue>{header.first, header.second});
        }
        return JSONValue(headers).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.headers: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.headers: unexpected native failure");
    }
}

JSValue FetchResponse::getURL(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        std::expected<FetchResponse*, JSValue> fetchResponseResult = getFetchResponse(jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return fetchResponseResult.error();
        }
        const FetchResponse* fetchResponse = fetchResponseResult.value();
        const FetchClient* fetchClient = fetchResponse->fetchClient;
        if (fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getURL: response is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.getURL: response state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse->curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED && responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getURL: response not ready");
        }
        return JS_NewStringLen(jsContext, fetchState.effectiveURL.data(), fetchState.effectiveURL.size());
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.url: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.url: unexpected native failure");
    }
}

JSValue FetchClient::send(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) {
    FetchClient* fetchClient = nullptr;
    CURL* curlEasyHandle = nullptr;
    try {
        if (fetchClient->pendingError->hasValue()) {
            return JS_ThrowInternalError(jsContext, "FetchClient.send: native failure");
        }
        const void* opaque = JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0]));
        if (opaque == nullptr) {
            return JS_EXCEPTION;
        }
        fetchClient = static_cast<const void_ptr::VoidPtr*>(opaque)->cast<FetchClient*>();
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowInternalError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(jsContext, "send requires a FetchRequest object");
        }
        FetchRequest fetchRequest;
        const owned::JSValue url(jsContext, JS_GetPropertyStr(jsContext, argv[0], "url"));
        const owned::JSValue method(jsContext, JS_GetPropertyStr(jsContext, argv[0], "method"));
        const owned::JSValue headers(jsContext, JS_GetPropertyStr(jsContext, argv[0], "headers"));
        const owned::JSValue body(jsContext, JS_GetPropertyStr(jsContext, argv[0], "body"));
        if (JS_IsException(url.get()) || JS_IsException(method.get()) || JS_IsException(headers.get()) || JS_IsException(body.get())) {
            return JS_EXCEPTION;
        }
        if (!JS_IsString(url.get()) || !JS_IsString(method.get()) || !JS_IsArray(headers.get()) || (!JS_IsString(body.get()) && JS_GetTypedArrayType(body.get()) != JS_TYPED_ARRAY_UINT8)) {
            return JS_ThrowTypeError(jsContext, "FetchRequest requires string url/method, header pairs, and string or Uint8Array body");
        }
        const std::optional<std::string> urlText = jsstring(jsContext, url.get());
        const std::optional<std::string> methodText = jsstring(jsContext, method.get());
        if (!urlText.has_value() || !methodText.has_value()) {
            return JS_EXCEPTION;
        }
        fetchRequest.url = urlText.value();
        fetchRequest.method = methodText.value();
        if (fetchRequest.url.empty() || fetchRequest.url.size() > (64U << 10U /* 64 KiB */) || fetchRequest.url.find('\0') != std::string::npos) {
            return JS_ThrowTypeError(jsContext, "Invalid or oversized HTTP URL");
        }
        if (fetchRequest.method.empty() || fetchRequest.method.size() > 64 || fetchRequest.method.find_first_not_of("!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos || fetchRequest.method == "CONNECT" || fetchRequest.method == "TRACE" || fetchRequest.method == "TRACK") {
            return JS_ThrowTypeError(jsContext, "Invalid HTTP method");
        }
        // Curl's URL parser independently enforces the normalized facade contract.
        const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> curlURL(curl_url(), curl_url_cleanup);
        if (curlURL == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        if (curl_url_set(curlURL.get(), CURLUPART_URL, fetchRequest.url.c_str(), 0) != CURLUE_OK) {
            return JS_ThrowTypeError(jsContext, "Invalid HTTP URL");
        }
        char* component = nullptr;
        CURLUcode urlStatus = curl_url_get(curlURL.get(), CURLUPART_SCHEME, &component, 0);
        const std::unique_ptr<char, decltype(&curl_free)> scheme(component, curl_free);
        if (urlStatus != CURLUE_OK || (std::string_view(scheme.get()) != "http" && std::string_view(scheme.get()) != "https")) {
            return JS_ThrowTypeError(jsContext, "Only HTTP(S) URLs are supported");
        }
        for (const CURLUPart part : {CURLUPART_USER, CURLUPART_PASSWORD}) {
            component = nullptr;
            urlStatus = curl_url_get(curlURL.get(), part, &component, 0);
            const std::unique_ptr<char, decltype(&curl_free)> credential(component, curl_free);
            if (urlStatus == CURLUE_OK) {
                return JS_ThrowTypeError(jsContext, "URL credentials are not supported");
            }
        }
        const owned::JSValue headerCountValue(jsContext, JS_GetPropertyStr(jsContext, headers.get(), "length"));
        uint32_t headerCount = 0;
        if (JS_IsException(headerCountValue.get()) || JS_ToUint32(jsContext, &headerCount, headerCountValue.get()) < 0) {
            return JS_EXCEPTION;
        }
        if (headerCount > (64U << 10U /* 64 KiB */) / 4) {
            return JS_ThrowRangeError(jsContext, "Too many request headers");
        }
        std::size_t headerBytes = 0;
        for (uint32_t index = 0; index < headerCount; index++) {
            const owned::JSValue pair(jsContext, JS_GetPropertyUint32(jsContext, headers.get(), index));
            if (JS_IsException(pair.get())) {
                return JS_EXCEPTION;
            }
            if (!JS_IsArray(pair.get())) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const owned::JSValue pairLengthValue(jsContext, JS_GetPropertyStr(jsContext, pair.get(), "length"));
            uint32_t pairLength = 0;
            if (JS_IsException(pairLengthValue.get()) || JS_ToUint32(jsContext, &pairLength, pairLengthValue.get()) < 0) {
                return JS_EXCEPTION;
            }
            if (pairLength != 2) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const owned::JSValue name(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 0));
            const owned::JSValue value(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 1));
            if (JS_IsException(name.get()) || JS_IsException(value.get())) {
                return JS_EXCEPTION;
            }
            if (!JS_IsString(name.get()) || !JS_IsString(value.get())) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const std::optional<std::string> nameText = jsstring(jsContext, name.get());
            const std::optional<std::string> valueText = jsstring(jsContext, value.get());
            if (!nameText.has_value() || !valueText.has_value()) {
                return JS_EXCEPTION;
            }
            if (nameText.value().empty() || nameText.value().find_first_not_of("!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) {
                return JS_ThrowTypeError(jsContext, "Invalid HTTP header name");
            }
            for (const char character : valueText.value()) {
                const unsigned char byte = static_cast<unsigned char>(character);
                if ((byte < 32 && byte != 9) || byte == 127) {
                    return JS_ThrowTypeError(jsContext, "Invalid HTTP header value");
                }
            }
            if (nameText.value().size() > (64U << 10U /* 64 KiB */) - headerBytes) {
                return JS_ThrowRangeError(jsContext, "Request headers exceed 64 KiB");
            }
            headerBytes += nameText.value().size();
            if (valueText.value().size() > (64U << 10U /* 64 KiB */) - headerBytes || (64U << 10U /* 64 KiB */) - headerBytes - valueText.value().size() < 4) {
                return JS_ThrowRangeError(jsContext, "Request headers exceed 64 KiB");
            }
            headerBytes += valueText.value().size() + 4;
            fetchRequest.headers.push_back({nameText.value(), valueText.value()});
        }
        if (JS_IsString(body.get())) {
            const std::optional<std::string> bodyText = jsstring(jsContext, body.get());
            if (!bodyText.has_value()) {
                return JS_EXCEPTION;
            }
            if (bodyText.value().size() > requestBodyLimit) {
                return JS_ThrowRangeError(jsContext, "Upload exceeds 32 MiB");
            }
            if (!bodyText.value().empty() && (fetchRequest.method == "GET" || fetchRequest.method == "HEAD")) {
                return JS_ThrowTypeError(jsContext, "GET/HEAD cannot have a body");
            }
            fetchRequest.body.assign(bodyText.value().begin(), bodyText.value().end());
        } else {
            std::size_t byteOffset = 0;
            std::size_t byteLength = 0;
            const owned::JSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, body.get(), &byteOffset, &byteLength, nullptr));
            if (JS_IsException(buffer.get())) {
                return JS_EXCEPTION;
            }
            if (!JS_IsArrayBuffer(buffer.get())) {
                return JS_ThrowTypeError(jsContext, "Shared request bodies are not supported");
            }
            std::size_t bufferLength = 0;
            const uint8_t* bytes = JS_GetArrayBuffer(jsContext, &bufferLength, buffer.get());
            if (bytes == nullptr && JS_HasException(jsContext)) {
                return JS_EXCEPTION;
            }
            if (byteOffset > bufferLength || byteLength > bufferLength - byteOffset || byteLength > requestBodyLimit) {
                return JS_ThrowRangeError(jsContext, "Invalid request body bounds or upload exceeds 32 MiB");
            }
            if (byteLength != 0 && (fetchRequest.method == "GET" || fetchRequest.method == "HEAD")) {
                return JS_ThrowTypeError(jsContext, "GET/HEAD cannot have a body");
            }
            if (byteLength != 0) {
                if (bytes == nullptr) {
                    return JS_ThrowInternalError(jsContext, "Missing request body storage");
                }
                fetchRequest.body.assign(bytes + byteOffset, bytes + byteOffset + byteLength);
            }
        }
        if (fetchClient->fetchStateMap.size() >= 64) {
            return JS_ThrowRangeError(jsContext, "Too many open FetchResponses");
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> ownedCurlEasyHandle(curl_easy_init(), curl_easy_cleanup);
        if (ownedCurlEasyHandle == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        fetchClient->fetchStateMap.try_emplace(ownedCurlEasyHandle.get());
        FetchState& fetchState = fetchClient->fetchStateMap.at(ownedCurlEasyHandle.get());
        fetchState.selfVoidPtr = std::make_unique<void_ptr::VoidPtr>(&fetchState);
        fetchClient->fetchStateIDCounter++;
        fetchState.fetchStateID = fetchClient->fetchStateIDCounter;
        fetchState.request = std::move(fetchRequest);
        curlEasyHandle = ownedCurlEasyHandle.get();
        fetchState.ownedCurlEasyHandle = ownedCurlEasyHandle.release();
        bool hasContentType = false;
        for (const std::pair<std::string, std::string>& header : fetchState.request.headers) {
            std::string lowerName = header.first;
            for (char& character : lowerName) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (lowerName == "content-type") {
                hasContentType = true;
            }
            std::string line = header.first + ": " + header.second;
            if (header.second.empty()) {
                line = header.first + ";"; // Curl's syntax for an explicitly empty header.
            }
            curl_slist* curlHeaders = curl_slist_append(fetchState.ownedCurlRequestHeaders, line.c_str());
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.ownedCurlRequestHeaders = curlHeaders;
        }
        if (!hasContentType) {
            // POSTFIELDS otherwise invents application/x-www-form-urlencoded.
            // The JS facade supplies a content type for strings, but not bytes.
            curl_slist* curlHeaders = curl_slist_append(fetchState.ownedCurlRequestHeaders, "Content-Type:");
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.ownedCurlRequestHeaders = curlHeaders;
        }
        CURLcode curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_URL, fetchState.request.url.c_str());
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_PROTOCOLS_STR, "http,https");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_FOLLOWLOCATION, CURLFOLLOW_OBEYCODE);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_MAXREDIRS, 20L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOSIGNAL, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYPEER, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYHOST, 2L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_ACCEPT_ENCODING, "");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HTTPHEADER, fetchState.ownedCurlRequestHeaders);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERDATA, fetchState.selfVoidPtr.get());
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERFUNCTION, [](char* buffer, std::size_t size, std::size_t nitems, void* userdata) noexcept {
            try {
                return curlHeaderFunction(buffer, size, nitems, userdata);
            } catch (...) {
                return static_cast<std::size_t>(CURL_WRITEFUNC_ERROR);
            }
        });
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEDATA, fetchState.selfVoidPtr.get());
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEFUNCTION, [](char* bytes, std::size_t size, std::size_t count, void* userdata) noexcept {
            try {
                return curlWriteFunction(bytes, size, count, userdata);
            } catch (...) {
                return static_cast<std::size_t>(CURL_WRITEFUNC_ERROR);
            }
        });
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        CURLcode methodStatus = CURLE_OK;
        if (fetchState.request.method == "HEAD") {
            methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOBODY, 1L);
        } else if (fetchState.request.method != "GET") {
            const char* bodyBytes = "";
            if (!fetchState.request.body.empty()) {
                bodyBytes = reinterpret_cast<const char*>(fetchState.request.body.data());
            }
            methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDS, bodyBytes);
            if (methodStatus == CURLE_OK) {
                methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(fetchState.request.body.size()));
            }
            if (methodStatus == CURLE_OK && fetchState.request.method != "POST") {
                methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_CUSTOMREQUEST, fetchState.request.method.c_str());
            }
        }
        if (methodStatus != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(methodStatus));
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        owned::JSValue ready(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(ready.get())) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        fetchState.responseReadyPromise = owned::JSPromise{
            .promise = std::move(ready),
            .resolveFunction = owned::JSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = owned::JSValue(jsContext, resolvingFunctions.at(1)),
        };
        owned::JSValue response(jsContext, JS_NewObjectClass(jsContext, fetchResponseClassID));
        if (JS_IsException(response.get())) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        std::unique_ptr<FetchResponse> fetchResponse = std::make_unique<FetchResponse>();
        fetchResponse->fetchClient = fetchClient;
        fetchResponse->curlEasyHandle = curlEasyHandle;
        void_ptr::VoidPtr* voidPtr = new void_ptr::VoidPtr(std::move(fetchResponse));
        if (JS_SetOpaque(response.get(), voidPtr) < 0) {
            delete voidPtr;
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_ThrowInternalError(jsContext, "Could not attach FetchResponse");
        }
        if (JS_DefinePropertyValueStr(jsContext, response.get(), "ready", JS_DupValue(jsContext, fetchState.responseReadyPromise.promise.get()), JS_PROP_ENUMERABLE) < 0) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        const CURLMcode curlMCode = curl_multi_add_handle(
            fetchClient->ownedCurlMultiHandle,
            fetchState.ownedCurlEasyHandle
        );
        if (curlMCode != CURLM_OK) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_ThrowInternalError(jsContext, "curl_multi_add_handle: %s", curl_multi_strerror(curlMCode));
        }
        fetchState.curlMultiHandle = fetchClient->ownedCurlMultiHandle;
        fetchState.responseCurlTransferStatus = CurlTransferStatus::Running;
        return response.release();
    } catch (const std::bad_alloc&) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: %s", exception.what());
    } catch (...) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError->hasValue()) {
                if (closeError.hasValue()) {
                    *fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: unexpected native failure");
    }
}

FetchState::~FetchState() {
    std::ignore = close();
}

errors::Error FetchState::close() noexcept {
    errors::Error error;
    if (curlMultiHandle != nullptr) {
        const CURLMcode curlMCode = curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle);
        if (curlMCode != CURLM_OK) {
            if (curlMCode != CURLM_ABORTED_BY_CALLBACK && curlMCode != CURLM_OUT_OF_MEMORY) {
                std::terminate();
            }
            error.errorCode = std::error_code(static_cast<int>(curlMCode), curlMultiErrorCategory);
        }
        curlMultiHandle = nullptr;
    }
    if (ownedCurlEasyHandle != nullptr) {
        curl_easy_cleanup(ownedCurlEasyHandle);
        ownedCurlEasyHandle = nullptr;
    }
    curl_slist_free_all(ownedCurlRequestHeaders);
    ownedCurlRequestHeaders = nullptr;
    selfVoidPtr.reset();
    responseReadyPromise = {};
    responseReadPromise = std::nullopt;
    return error;
}

FetchClient::FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, errors::Error& pendingError, std::function<void(JSContext*)> jsExecutePendingJobs) {
    if (jsContext == nullptr || uvLoop == nullptr || !jsExecutePendingJobs) {
        throw std::invalid_argument("FetchClient requires a context, initialized loop, and completion hook");
    }
    this->jsRuntime = JS_GetRuntime(jsContext);
    this->jsContext = jsContext;
    this->uvLoop = uvLoop;
    this->pendingError = &pendingError;
    this->jsExecutePendingJobs = std::move(jsExecutePendingJobs);
    this->ownerThreadID = this_thread::get_id();
    try {
        static std::once_flag curlInitialization;
        std::call_once(curlInitialization, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                throw std::runtime_error("curl_global_init failed");
            }
        });
        this->selfVoidPtr = std::make_unique<void_ptr::VoidPtr>(this);
        ownedCurlMultiHandle = curl_multi_init();
        if (ownedCurlMultiHandle == nullptr) {
            throw std::bad_alloc();
        }
        {
            owned::UvHandle<uv_timer_t> uvTimerHandle(std::make_unique<uv_timer_t>());
            const int status = uv_timer_init(uvLoop, uvTimerHandle.get());
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvTimerHandle.setInitialized();
            uvTimerHandle.get()->data = new void_ptr::VoidPtr(this);
            this->uvTimerHandle = std::move(uvTimerHandle);
        }
        {
            owned::UvHandle<uv_async_t> uvAsyncHandle(std::make_unique<uv_async_t>());
            const auto uvAsyncCallback = [](uv_async_t* uvAsyncHandle) noexcept {
                FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(uvAsyncHandle->data)->cast<FetchClient*>();
                uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle));
                // Another callback may have recorded an error earlier in this iteration.
                if (fetchClient->pendingError->hasValue()) {
                    return;
                }
                try {
                    settlePendingPromises(fetchClient);
                } catch (...) {
                    if (!fetchClient->pendingError->hasValue()) {
                        fetchClient->pendingError->exceptionPtr = std::current_exception();
                    }
                }
            };
            const int status = uv_async_init(uvLoop, uvAsyncHandle.get(), uvAsyncCallback);
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvAsyncHandle.setInitialized();
            uvAsyncHandle.get()->data = new void_ptr::VoidPtr(this);
            uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle.get()));
            this->uvAsyncHandle = std::move(uvAsyncHandle);
        }
        CURLMcode curlMCode = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_SOCKETFUNCTION, [](CURL* curlEasyHandle, curl_socket_t socket, int action, void* userdata, void* socketData) noexcept {
            try {
                return curlSocketFunction(curlEasyHandle, socket, action, userdata, socketData);
            } catch (...) {
                const FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(userdata)->cast<FetchClient*>();
                if (!fetchClient->pendingError->hasValue()) {
                    fetchClient->pendingError->exceptionPtr = std::current_exception();
                }
                return -1;
            }
        });
        if (curlMCode != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(curlMCode));
        }
        curlMCode = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_SOCKETDATA, this->selfVoidPtr.get());
        if (curlMCode != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(curlMCode));
        }
        curlMCode = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_TIMERFUNCTION, [](CURLM* curlMultiHandle, long milliseconds, void* userdata) noexcept {
            try {
                return curlTimerFunction(curlMultiHandle, milliseconds, userdata);
            } catch (...) {
                const FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(userdata)->cast<FetchClient*>();
                if (!fetchClient->pendingError->hasValue()) {
                    fetchClient->pendingError->exceptionPtr = std::current_exception();
                }
                return -1;
            }
        });
        if (curlMCode != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(curlMCode));
        }
        curlMCode = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_TIMERDATA, this->selfVoidPtr.get());
        if (curlMCode != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(curlMCode));
        }
    } catch (...) {
        std::ignore = close();
        throw;
    }
}

FetchClient::~FetchClient() {
    std::ignore = close();
}

errors::Error FetchClient::close() noexcept {
    if (this_thread::get_id() != ownerThreadID) {
        std::terminate();
    }
    if (closed) {
        return {};
    }
    closed = true;
    errors::Error error = *pendingError;
    for (std::pair<CURL* const, FetchState>& entry : fetchStateMap) {
        const errors::Error closeError = entry.second.close();
        if (!error.hasValue()) {
            if (pendingError->hasValue()) {
                error = *pendingError;
            } else if (closeError.hasValue()) {
                error = closeError;
            }
        }
    }
    fetchStateMap.clear();
    if (ownedCurlMultiHandle != nullptr) {
        const CURLMcode curlMCode = curl_multi_cleanup(ownedCurlMultiHandle);
        if (curlMCode != CURLM_OK) {
            std::terminate();
        }
        ownedCurlMultiHandle = nullptr;
    }
    if (!error.hasValue()) {
        if (pendingError->hasValue()) {
            error = *pendingError;
        }
    }
    // Multi cleanup can invoke socket callbacks; release their dependencies last.
    selfVoidPtr.reset();
    uvPollHandleMap.clear();
    uvTimerHandle.reset();
    uvAsyncHandle.reset();
    return error;
}

JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient) {
    try {
        if (this_thread::get_id() != fetchClient.ownerThreadID) {
            std::terminate();
        }
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        JSClassID clientClassID = 0;
        JS_NewClassID(jsRuntime, &clientClassID);
        const JSClassDef clientClass = {
            .class_name = "FetchClient",
            .finalizer = [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                delete static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value, JS_GetClassID(value)));
            },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, clientClassID, &clientClass) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        JSClassID fetchResponseClassID = 0;
        {
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (fetchResponseClassIDMap.contains(jsRuntime)) {
                fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
            } else {
                JS_NewClassID(jsRuntime, &fetchResponseClassID);
                fetchResponseClassIDMap.insert_or_assign(jsRuntime, fetchResponseClassID);
                const auto jsRuntimeFinalizer = [](JSRuntime* jsRuntime, void* /* argument */) {
                    const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
                    fetchResponseClassIDMap.erase(jsRuntime);
                };
                const int status = JS_AddRuntimeFinalizer(jsRuntime, jsRuntimeFinalizer, nullptr);
                if (status < 0) {
                    fetchResponseClassIDMap.erase(jsRuntime);
                    throw std::bad_alloc();
                }
            }
        }
        const JSClassDef responseClass = {
            .class_name = "FetchResponse",
            .finalizer = [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                FetchClient* fetchClient = nullptr;
                try {
                    const std::unique_ptr<void_ptr::VoidPtr> voidPtr(static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value, JS_GetClassID(value))));
                    if (voidPtr == nullptr) {
                        return;
                    }
                    const FetchResponse& fetchResponse = voidPtr->cast<FetchResponse>();
                    if (fetchResponse.curlEasyHandle == nullptr) {
                        return;
                    }
                    fetchClient = fetchResponse.fetchClient;
                    // Context destruction can run this finalizer after client
                    // close(). The cleared map proves no work remains; do not
                    // touch the loop, async handle, or mailbox in that case.
                    if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
                        return;
                    }
                    // An earlier failure already requires host cleanup; do not
                    // schedule another settlement pass from this finalizer.
                    if (fetchClient->pendingError->hasValue()) {
                        return;
                    }
                    // GC can run while allocating promise results. Do
                    // not erase a FetchState currently being accessed
                    // by settlePendingPromises(), and do not settle promises
                    // from inside a QuickJS finalizer.
                    fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle).abandoned = true;
                    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient->uvAsyncHandle.get()));
                    const int status = uv_async_send(fetchClient->uvAsyncHandle.get());
                    if (status != 0) {
                        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient->uvAsyncHandle.get()));
                        if (!fetchClient->pendingError->hasValue()) {
                            fetchClient->pendingError->errorCode = std::error_code(status, uvErrorCategory);
                        }
                    }
                } catch (...) {
                    if (fetchClient == nullptr) {
                        std::terminate(); // No reporter means invalid callback storage.
                    }
                    if (!fetchClient->pendingError->hasValue()) {
                        fetchClient->pendingError->exceptionPtr = std::current_exception();
                    }
                }
            },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (!JS_IsRegisteredClass(jsRuntime, fetchResponseClassID) && JS_NewClass(jsRuntime, fetchResponseClassID, &responseClass) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        owned::JSValue client(jsContext, JS_NewObjectClass(jsContext, clientClassID));
        if (JS_IsException(client.get())) {
            return nullptr;
        }
        void_ptr::VoidPtr* voidPtr = new void_ptr::VoidPtr(&fetchClient);
        if (JS_SetOpaque(client.get(), voidPtr) < 0) {
            delete voidPtr;
            JS_ThrowInternalError(jsContext, "Could not attach FetchClient");
            return nullptr;
        }
        JSValueConst clientData = client.get();
        const JSValue send = JS_NewCFunctionData2(jsContext, FetchClient::send, "send", 1, 0, 1, &clientData);
        if (JS_IsException(send) || JS_DefinePropertyValueStr(jsContext, client.get(), "send", send, JS_PROP_ENUMERABLE) < 0) {
            return nullptr;
        }
        owned::JSValue prototype(jsContext, JS_NewObject(jsContext));
        if (JS_IsException(prototype.get())) {
            return nullptr;
        }
        const std::array<std::pair<const char*, JSCFunction*>, 2> methods = {{
            {"read", FetchResponse::read},
            {"close", FetchResponse::close},
        }};
        for (const std::pair<const char*, JSCFunction*>& method : methods) {
            const JSValue function = JS_NewCFunction(jsContext, method.second, method.first, 0);
            if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, prototype.get(), method.first, function, 0) < 0) {
                return nullptr;
            }
        }
        const std::array<std::pair<const char*, JSCFunction*>, 3> getters = {{
            {"status", FetchResponse::getStatus},
            {"headers", FetchResponse::getHeaders},
            {"url", FetchResponse::getURL},
        }};
        for (const std::pair<const char*, JSCFunction*>& getter : getters) {
            const JSValue function = JS_NewCFunction(jsContext, getter.second, getter.first, 0);
            if (JS_IsException(function)) {
                return nullptr;
            }
            const JSAtom atom = JS_NewAtom(jsContext, getter.first);
            const int status = JS_DefinePropertyGetSet(jsContext, prototype.get(), atom, function, JS_UNDEFINED, JS_PROP_ENUMERABLE);
            JS_FreeAtom(jsContext, atom);
            if (status < 0) {
                return nullptr;
            }
        }
        JS_SetClassProto(jsContext, fetchResponseClassID, prototype.release());
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:fetch_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            const JSValue client = JS_GetModulePrivateValue(jsContext, jsModuleDef);
            if (JS_IsException(client)) {
                return -1;
            }
            return JS_SetModuleExport(jsContext, jsModuleDef, "fetchClient", client);
        });
        if (jsModuleDef == nullptr || JS_AddModuleExport(jsContext, jsModuleDef, "fetchClient") < 0 || JS_SetModulePrivateValue(jsContext, jsModuleDef, client.release()) < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(jsContext);
        return nullptr;
    } catch (const std::exception& exception) {
        JS_ThrowInternalError(jsContext, "createFetchImplModule: %s", exception.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(jsContext, "createFetchImplModule: unexpected native failure");
        return nullptr;
    }
}

} // namespace amj::javascript
