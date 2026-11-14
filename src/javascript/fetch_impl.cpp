#include "fetch_impl.hpp"

#include "errors/errors.hpp"
#include "intlimit/intlimit.hpp"
#include "javascript.hpp"
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
#include <utility>
#include <variant>
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

std::mutex fetchResponseClassIDMutex = {};
std::map<JSRuntime*, JSClassID> fetchResponseClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

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

// Passed as CURLOPT_WRITEFUNCTION  to read the response body.
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
        fetchState->responseStatus = CurlTransferStatus::Paused;
        return CURL_WRITEFUNC_PAUSE;
    }

    // Else append the bytes to the responseBuffer.
    fetchState->responseBuffer.push_back(std::vector<uint8_t>(ptr, ptr + realsize));
    fetchState->responseBufferSize += realsize;
    return realsize;
}

// Drive curl transfers, then mark the ones that have completed. On a loop
// failure, leave transfers registered for host-coordinated shutdown and return.
// The outer callback adapter stops the loop after curl unwinds.
void driveCurlTransfers(FetchClient& fetchClient, curl_socket_t socket, int eventBitmask) {
    // Callers check pendingError at the libuv boundary or after the last curl
    // operation in the unpause path. No callbacks run between that check and here.
    // Tell curl to read and write socket data.
    int runningHandles = 0;
    const CURLMcode curlMCode = curl_multi_socket_action(fetchClient.ownedCurlMultiHandle, socket, eventBitmask, &runningHandles);
    // Preserve the original error if a curl callback already recorded one.
    if (hasError(fetchClient.pendingError)) {
        return;
    }
    if (curlMCode != CURLM_OK) {
        *fetchClient.pendingError = std::error_code(static_cast<int>(curlMCode), curlMultiErrorCategory);
        return;
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
        fetchState.responseStatus = CurlTransferStatus::Completed;
        if (curlMsg->data.result != CURLE_OK) {
            fetchState.error.errorCode = errors::ErrorCode::GenericError;
            if (!fetchState.error.errorMessage.empty()) {
                fetchState.error.errorMessage.append(": ");
            }
            fetchState.error.errorMessage.append(curl_easy_strerror(curlMsg->data.result));
        }
        if (!fetchState.responseHeadersComplete) {
            fetchState.error.errorCode = errors::ErrorCode::GenericError;
            if (!fetchState.error.errorMessage.empty()) {
                fetchState.error.errorMessage.append(": ");
            }
            fetchState.error.errorMessage.append("HTTP response ended without final headers");
        }
    }

    // Signal the uv async handle, which triggers settlePendingPromises().
    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle.get()));
    const int status = uv_async_send(fetchClient.uvAsyncHandle.get());
    if (status != 0) {
        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle.get()));
        // Nothing since the post-curl checkpoint can invoke JS/curl callbacks;
        // this is the first error, so no second mailbox check is needed.
        *fetchClient.pendingError = std::error_code(status, uvErrorCategory);
        return;
    }
}

// Called only by the async adapter, which checks pendingError before entry.
// QuickJS allocations can run GC finalizers, and resolving an object can invoke
// a user-defined `then` getter. Both can report errors, so check before pumping
// more JS jobs. The loop checkpoint then checks errors reported by those jobs.
void settlePendingPromises(FetchClient* fetchClient) {
    // Snapshot the fetchStateMap's CURL* keys + fetchStateID into an entries
    // list before looping over the entries. The fetchStateMap can be modified
    // during iteration, so we need a stable list to iterate over.
    std::vector<std::pair<CURL*, uint64_t>> entries;
    entries.reserve(fetchClient->fetchStateMap.size());
    for (const std::pair<CURL* const, FetchState>& entry : fetchClient->fetchStateMap) {
        entries.push_back({entry.first, entry.second.fetchStateID});
    }

    // A previous iteration may have run JS jobs or destroyed an extracted
    // FetchState (which invokes curl callbacks). Check before the next entry.
    for (const std::pair<CURL*, uint64_t>& entry : entries) {
        if (hasError(fetchClient->pendingError)) {
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
            const FetchState& fetchState = mapNode.mapped();
            const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadyPromise.promise.get());
            if (readyPromiseState != JS_PROMISE_PENDING && !fetchState.responseReadPromise.has_value()) {
                continue;
            }
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "AbortError", "FetchResponse is closed");
            // Reject responseReadyPromise.
            if (readyPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
            }
            // Reject responseReadPromise.
            if (fetchState.responseReadPromise.has_value()) {
                const JSPromiseStateEnum readPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadPromise.value().promise.get());
                if (readPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                    JS_FreeValue(fetchClient->jsContext, result);
                }
            }
            if (hasError(fetchClient->pendingError)) {
                return;
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // If responseReadyPromise has not yet been settled, settle it first.
        const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadyPromise.promise.get());
        if (readyPromiseState != JS_PROMISE_FULFILLED && readyPromiseState != JS_PROMISE_REJECTED) {
            if (!fetchState.responseHeadersComplete) {
                // The transfer has not yet completed, don't settle the
                // responseReadyPromise.
                if (fetchState.responseStatus != CurlTransferStatus::Completed) {
                    continue;
                }

                // The transfer completed without receiving any response
                // headers. Reject responseReadyPromise.
                OwnedJSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.error.errorMessage);
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
                if (hasError(fetchClient->pendingError)) {
                    return;
                }
                fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
                continue;
            }

            // Resolve resonseReadyPromise.
            const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.resolveFunction.get(), JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(fetchClient->jsContext, result);
            if (hasError(fetchClient->pendingError)) {
                return;
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // No responseReadPromise needs settling.
        if (!fetchState.responseReadPromise.has_value()) {
            continue;
        }

        // No error, buffered data, or end-of-stream to settle the
        // responseReadPromise with.
        if (!fetchState.error.errorCode && fetchState.responseBuffer.empty() && fetchState.responseStatus != CurlTransferStatus::Completed) {
            continue;
        }

        // Move fetchState.responseReadPromise out to a local variable and
        // reset it so JS can request another read.
        OwnedJSPromise responseReadPromise = std::move(fetchState.responseReadPromise.value());
        fetchState.responseReadPromise.reset();

        // Reject the responseReadPromise if the transfer encountered an error.
        if (fetchState.error.errorCode) {
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.error.errorMessage);
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
            JS_FreeValue(fetchClient->jsContext, result);
            if (hasError(fetchClient->pendingError)) {
                return;
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // No error or buffered chunks remain, and the transfer has completed.
        // Resolve the responseReadPromise with null to indicate end-of-stream.
        if (fetchState.responseBuffer.empty()) {
            JSValue jsNull = JS_NULL;
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &jsNull);
            JS_FreeValue(fetchClient->jsContext, result);
            if (hasError(fetchClient->pendingError)) {
                return;
            }
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
        if (fetchState.responseStatus == CurlTransferStatus::Paused && fetchState.responseBufferSize <= bodyBufferLimit / 2) {
            fetchState.responseStatus = CurlTransferStatus::Running;
            const CURLcode curlCode = curl_easy_pause(curlEasyHandle, CURLPAUSE_CONT);
            // Unpausing can synchronously invoke curl callbacks. Do not remove
            // handles or drive curl again if one of them reported a loop error.
            if (hasError(fetchClient->pendingError)) {
                fetchState.responseReadPromise = std::move(responseReadPromise);
                return;
            }
            if (curlCode != CURLE_OK) {
                fetchState.error = errors::message(curl_easy_strerror(curlCode));
                fetchState.responseStatus = CurlTransferStatus::Completed;
                if (fetchState.curlMultiHandle != nullptr) {
                    const CURLMcode curlMCode = curl_multi_remove_handle(fetchState.curlMultiHandle, curlEasyHandle);
                    if (curlMCode != CURLM_OK) {
                        if (!hasError(fetchClient->pendingError)) {
                            *fetchClient->pendingError = std::error_code(static_cast<int>(curlMCode), curlMultiErrorCategory);
                        }
                        // Preserve callback storage and the pending promise
                        // until the host can safely coordinate shutdown.
                        fetchState.responseReadPromise = std::move(responseReadPromise);
                        return;
                    }
                    fetchState.curlMultiHandle = nullptr;
                    // Removal also invokes curl callbacks; its return code is
                    // not the mailbox, which retains their original failure.
                    if (hasError(fetchClient->pendingError)) {
                        fetchState.responseReadPromise = std::move(responseReadPromise);
                        return;
                    }
                }
            }
            try {
                driveCurlTransfers(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
            } catch (...) {
                fetchState.responseReadPromise = std::move(responseReadPromise);
                throw;
            }
            if (hasError(fetchClient->pendingError)) {
                fetchState.responseReadPromise = std::move(responseReadPromise);
                return;
            }
        }

        // Convert the chunk into a JS Uint8Array.
        OwnedJSValue uint8Array(fetchClient->jsContext, JS_NewUint8ArrayCopy(fetchClient->jsContext, chunk.data(), chunk.size()));
        if (JS_IsException(uint8Array.get())) {
            OwnedJSValue error(fetchClient->jsContext, JS_GetException(fetchClient->jsContext));
            const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
            JS_FreeValue(fetchClient->jsContext, result);
            if (hasError(fetchClient->pendingError)) {
                return;
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        // Reject the responseReadPromise with the Uint8Array chunk.
        const JSValue result = JS_Call(fetchClient->jsContext, responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &uint8Array.get());
        JS_FreeValue(fetchClient->jsContext, result);
        if (hasError(fetchClient->pendingError)) {
            return;
        }
        fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
    }
}

int curlSocketFunction(CURL* /* curlEasyHandle */, curl_socket_t socket, int what, const void* clientp, void* /* socketp */) {
    struct PollData {
        FetchClient* fetchClient = nullptr;
        curl_socket_t socket = CURL_SOCKET_BAD;
    };
    FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(clientp)->cast<FetchClient*>();

    if (what == CURL_POLL_REMOVE) {
        fetchClient->uvPollHandleMap.erase(socket);
        return 0;
    }

    // Curl also invokes this while removing/cleaning handles. A pending error
    // must not block the watch updates needed to finish that cleanup. The host
    // keeps libuv handles alive until curl teardown has returned.
    if (!fetchClient->uvPollHandleMap.contains(socket)) {
        owned::UvHandle<uv_poll_t> uvPollHandle(std::make_unique<uv_poll_t>());
        const int status = uv_poll_init_socket(fetchClient->uvLoop, uvPollHandle.get(), socket);
        if (status != 0) {
            if (!hasError(fetchClient->pendingError)) {
                *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
            }
            return -1;
        }
        uvPollHandle.setInitialized();
        uvPollHandle.get()->data = new void_ptr::VoidPtr(
            std::make_unique<PollData>(PollData{
                .fetchClient = fetchClient,
                .socket = socket,
            })
        );
        fetchClient->uvPollHandleMap.insert_or_assign(socket, std::move(uvPollHandle));
    }

    unsigned int events = 0;
    if (what == CURL_POLL_IN || what == CURL_POLL_INOUT) {
        events |= static_cast<unsigned int>(UV_READABLE);
    }
    if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT) {
        events |= static_cast<unsigned int>(UV_WRITABLE);
    }
    uv_poll_t* uvPollHandle = fetchClient->uvPollHandleMap.at(socket).get();
    if (events == 0) {
        const int status = uv_poll_stop(uvPollHandle);
        if (status != 0 && !hasError(fetchClient->pendingError)) {
            *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
        }
        return status == 0 ? 0 : -1;
    }
    const int status = uv_poll_start(uvPollHandle, static_cast<int>(events), [](uv_poll_t* uvPollHandle, int status, int events) noexcept {
        const PollData& pollData = static_cast<void_ptr::VoidPtr*>(uvPollHandle->data)->cast<PollData>();
        FetchClient* fetchClient = pollData.fetchClient;
        // uv_stop does not discard other callbacks in the current iteration.
        if (hasError(fetchClient->pendingError)) {
            uv_stop(fetchClient->uvLoop);
            return;
        }
        try {
            unsigned int curlEvents = 0;
            if (status < 0) {
                curlEvents |= CURL_CSELECT_ERR;
            }
            if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_READABLE)) != 0) {
                curlEvents |= CURL_CSELECT_IN;
            }
            if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_WRITABLE)) != 0) {
                curlEvents |= CURL_CSELECT_OUT;
            }
            // driveCurlTransfers() may close this poll handle. Do not use pollData after it.
            driveCurlTransfers(*fetchClient, pollData.socket, static_cast<int>(curlEvents));
        } catch (...) {
            if (!hasError(fetchClient->pendingError)) {
                *fetchClient->pendingError = std::current_exception();
            }
        }
        if (hasError(fetchClient->pendingError)) {
            uv_stop(fetchClient->uvLoop);
        }
    });
    if (status != 0 && !hasError(fetchClient->pendingError)) {
        *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
    }
    return status == 0 ? 0 : -1;
}

int curlTimerFunction(CURLM* /* curlMultiHandle */, long milliseconds, const void* userdata) {
    const FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchClient*>();
    // Like socket updates, timer updates can be requested during curl teardown.
    // The timer remains initialized until all curl cleanup has returned.
    if (milliseconds < 0) {
        const int status = uv_timer_stop(fetchClient->uvTimerHandle.get());
        if (status != 0 && !hasError(fetchClient->pendingError)) {
            *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
        }
        return status == 0 ? 0 : -1;
    }
    uv_update_time(fetchClient->uvLoop);
    const int status = uv_timer_start(fetchClient->uvTimerHandle.get(), [](uv_timer_t* uvTimerHandle) noexcept {
        FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(uvTimerHandle->data)->cast<FetchClient*>();
        // Another callback may already have stopped this loop iteration.
        if (hasError(fetchClient->pendingError)) {
            uv_stop(fetchClient->uvLoop);
            return;
        }
        try {
            driveCurlTransfers(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
        } catch (...) {
            if (!hasError(fetchClient->pendingError)) {
                *fetchClient->pendingError = std::current_exception();
            }
        }
        if (hasError(fetchClient->pendingError)) {
            uv_stop(fetchClient->uvLoop);
        }
    },
        static_cast<uint64_t>(milliseconds), 0);
    if (status != 0 && !hasError(fetchClient->pendingError)) {
        *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
    }
    return status == 0 ? 0 : -1;
}

} // namespace

JSValue FetchResponse::read(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowTypeError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        const void* responseData = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
        if (responseData == nullptr) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(responseData)->cast<FetchResponse>();
        FetchClient* fetchClient = fetchResponse.fetchClient;
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
        }
        // JS can catch a native failure and call read again in the same job,
        // before the host regains control to inspect pendingError.
        if (hasError(fetchClient->pendingError)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse requires no pending loop error");
        }
        if (fetchState.responseReadPromise.has_value()) {
            return JS_ThrowTypeError(jsContext, "Only one outstanding read is supported");
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        OwnedJSValue promise(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(promise.get())) {
            return JS_EXCEPTION;
        }
        fetchState.responseReadPromise = OwnedJSPromise{
            .promise = std::move(promise),
            .resolveFunction = OwnedJSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = OwnedJSValue(jsContext, resolvingFunctions.at(1)),
        };
        uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient->uvAsyncHandle.get()));
        const int status = uv_async_send(fetchClient->uvAsyncHandle.get());
        if (status != 0) {
            uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient->uvAsyncHandle.get()));
            if (!hasError(fetchClient->pendingError)) {
                *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
            }
            return JS_ThrowInternalError(jsContext, "uv_async_send: %s", uv_strerror(status));
        }
        return JS_DupValue(jsContext, fetchState.responseReadPromise.value().promise.get());
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: unexpected native failure");
    }
}

JSValue FetchResponse::close(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowTypeError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        const void* responseData = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
        if (responseData == nullptr) {
            return JS_EXCEPTION;
        }
        FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(responseData)->cast<FetchResponse>();
        FetchClient* fetchClient = fetchResponse.fetchClient;
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        // A JS catch block can request close in the same job that reported an
        // error. Leave registered state intact for the host's recovery pass.
        if (hasError(fetchClient->pendingError)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse cleanup deferred until the host handles the loop error");
        }
        CURL* curlEasyHandle = fetchResponse.curlEasyHandle;
        fetchResponse.curlEasyHandle = nullptr;
        if (curlEasyHandle != nullptr) {
            const std::map<CURL*, FetchState>::node_type mapNode = fetchClient->fetchStateMap.extract(curlEasyHandle);
            if (mapNode.empty()) {
                return JS_UNDEFINED;
            }
            const FetchState& fetchState = mapNode.mapped();
            const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadyPromise.promise.get());
            if (!fetchState.responseReadPromise.has_value() && readyPromiseState != JS_PROMISE_PENDING) {
                return JS_UNDEFINED;
            }
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "AbortError", "FetchResponse is closed");
            if (readyPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
            }
            if (fetchState.responseReadPromise.has_value()) {
                const JSPromiseStateEnum readPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.responseReadPromise.value().promise.get());
                if (readPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(fetchClient->jsContext, fetchState.responseReadPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                    JS_FreeValue(fetchClient->jsContext, result);
                }
            }
            if (JS_HasException(jsContext)) {
                return JS_EXCEPTION;
            }
        }
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: unexpected native failure");
    }
}

JSValue FetchResponse::getStatus(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */) {
    try {
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowTypeError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        const void* responseData = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
        if (responseData == nullptr) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(responseData)->cast<FetchResponse>();
        const FetchClient* fetchClient = fetchResponse.fetchClient;
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
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
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowTypeError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        const void* responseData = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
        if (responseData == nullptr) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(responseData)->cast<FetchResponse>();
        const FetchClient* fetchClient = fetchResponse.fetchClient;
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
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
        JSClassID fetchResponseClassID = 0;
        {
            JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(fetchResponseClassIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowTypeError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        const void* responseData = JS_GetOpaque2(jsContext, thisValue, fetchResponseClassID);
        if (responseData == nullptr) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = static_cast<const void_ptr::VoidPtr*>(responseData)->cast<FetchResponse>();
        const FetchClient* fetchClient = fetchResponse.fetchClient;
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
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
    CURL* registeredHandle = nullptr;
    try {
        const void* clientData = JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0]));
        if (clientData == nullptr) {
            return JS_EXCEPTION;
        }
        fetchClient = static_cast<const void_ptr::VoidPtr*>(clientData)->cast<FetchClient*>();
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        // A caught JS exception does not end the current job: it can call send
        // again after an earlier native operation reported a loop error.
        if (jsContext != fetchClient->jsContext || hasError(fetchClient->pendingError)) {
            return JS_ThrowInternalError(jsContext, "FetchClient requires its live context and no pending loop error");
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
        const OwnedJSValue url(jsContext, JS_GetPropertyStr(jsContext, argv[0], "url"));
        const OwnedJSValue method(jsContext, JS_GetPropertyStr(jsContext, argv[0], "method"));
        const OwnedJSValue headers(jsContext, JS_GetPropertyStr(jsContext, argv[0], "headers"));
        const OwnedJSValue body(jsContext, JS_GetPropertyStr(jsContext, argv[0], "body"));
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
        const OwnedJSValue headerCountValue(jsContext, JS_GetPropertyStr(jsContext, headers.get(), "length"));
        uint32_t headerCount = 0;
        if (JS_IsException(headerCountValue.get()) || JS_ToUint32(jsContext, &headerCount, headerCountValue.get()) < 0) {
            return JS_EXCEPTION;
        }
        if (headerCount > (64U << 10U /* 64 KiB */) / 4) {
            return JS_ThrowRangeError(jsContext, "Too many request headers");
        }
        std::size_t headerBytes = 0;
        for (uint32_t index = 0; index < headerCount; index++) {
            const OwnedJSValue pair(jsContext, JS_GetPropertyUint32(jsContext, headers.get(), index));
            if (JS_IsException(pair.get())) {
                return JS_EXCEPTION;
            }
            if (!JS_IsArray(pair.get())) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const OwnedJSValue pairLengthValue(jsContext, JS_GetPropertyStr(jsContext, pair.get(), "length"));
            uint32_t pairLength = 0;
            if (JS_IsException(pairLengthValue.get()) || JS_ToUint32(jsContext, &pairLength, pairLengthValue.get()) < 0) {
                return JS_EXCEPTION;
            }
            if (pairLength != 2) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const OwnedJSValue name(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 0));
            const OwnedJSValue value(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 1));
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
            const OwnedJSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, body.get(), &byteOffset, &byteLength, nullptr));
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
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlEasyHandle(curl_easy_init(), curl_easy_cleanup);
        if (curlEasyHandle == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        fetchClient->fetchStateMap.try_emplace(curlEasyHandle.get());
        FetchState& fetchState = fetchClient->fetchStateMap.at(curlEasyHandle.get());
        fetchState.selfVoidPtr = std::make_unique<void_ptr::VoidPtr>(&fetchState);
        fetchClient->fetchStateIDCounter++;
        fetchState.fetchStateID = fetchClient->fetchStateIDCounter;
        fetchState.request = std::move(fetchRequest);
        registeredHandle = curlEasyHandle.get();
        fetchState.ownedCurlEasyHandle = curlEasyHandle.release();
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
        OwnedJSValue ready(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(ready.get())) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_EXCEPTION;
        }
        fetchState.responseReadyPromise = OwnedJSPromise{
            .promise = std::move(ready),
            .resolveFunction = OwnedJSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = OwnedJSValue(jsContext, resolvingFunctions.at(1)),
        };
        OwnedJSValue response(jsContext, JS_NewObjectClass(jsContext, fetchResponseClassID));
        if (JS_IsException(response.get())) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_EXCEPTION;
        }
        std::unique_ptr<FetchResponse> fetchResponse = std::make_unique<FetchResponse>();
        fetchResponse->fetchClient = fetchClient;
        fetchResponse->curlEasyHandle = registeredHandle;
        void_ptr::VoidPtr* voidPtr = new void_ptr::VoidPtr(std::move(fetchResponse));
        if (JS_SetOpaque(response.get(), voidPtr) < 0) {
            delete voidPtr;
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_ThrowInternalError(jsContext, "Could not attach FetchResponse");
        }
        if (JS_DefinePropertyValueStr(jsContext, response.get(), "ready", JS_DupValue(jsContext, fetchState.responseReadyPromise.promise.get()), JS_PROP_ENUMERABLE) < 0) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_EXCEPTION;
        }
        const CURLMcode curlMCode = curl_multi_add_handle(
            fetchClient->ownedCurlMultiHandle,
            fetchState.ownedCurlEasyHandle
        );
        if (curlMCode != CURLM_OK) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_ThrowInternalError(jsContext, "curl_multi_add_handle: %s", curl_multi_strerror(curlMCode));
        }
        fetchState.curlMultiHandle = fetchClient->ownedCurlMultiHandle;
        fetchState.responseStatus = CurlTransferStatus::Running;
        return response.release();
    } catch (const std::bad_alloc&) {
        if (registeredHandle != nullptr) {
            fetchClient->fetchStateMap.erase(registeredHandle);
        }
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (registeredHandle != nullptr) {
            fetchClient->fetchStateMap.erase(registeredHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: %s", exception.what());
    } catch (...) {
        if (registeredHandle != nullptr) {
            fetchClient->fetchStateMap.erase(registeredHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: unexpected native failure");
    }
}

FetchState::~FetchState() {
    if (curlMultiHandle != nullptr) {
        if (curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle) != CURLM_OK) {
            std::terminate(); // Never destroy callback storage still registered with curl.
        }
        curlMultiHandle = nullptr;
    }
    if (ownedCurlEasyHandle != nullptr) {
        curl_easy_cleanup(ownedCurlEasyHandle);
    }
    curl_slist_free_all(ownedCurlRequestHeaders);
}

FetchClient::FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::variant<std::error_code, std::exception_ptr>& pendingError, std::function<void(JSContext*)> jsExecutePendingJobs) {
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
                // Another callback may have reported an error earlier in this
                // iteration; uv_stop does not discard already-ready callbacks.
                if (hasError(fetchClient->pendingError)) {
                    uv_stop(fetchClient->uvLoop);
                    return;
                }
                try {
                    settlePendingPromises(fetchClient);
                } catch (...) {
                    if (!hasError(fetchClient->pendingError)) {
                        *fetchClient->pendingError = std::current_exception();
                    }
                }
                if (hasError(fetchClient->pendingError)) {
                    uv_stop(fetchClient->uvLoop);
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
                if (!hasError(fetchClient->pendingError)) {
                    *fetchClient->pendingError = std::current_exception();
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
                if (!hasError(fetchClient->pendingError)) {
                    *fetchClient->pendingError = std::current_exception();
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
        shutdown();
        throw;
    }
}

FetchClient::~FetchClient() {
    shutdown();
}

void FetchClient::shutdown() {
    if (this_thread::get_id() != ownerThreadID) {
        std::terminate();
    }
    if (shuttingDown) {
        return;
    }
    shuttingDown = true;
    fetchStateMap.clear();
    if (ownedCurlMultiHandle != nullptr) {
        if (curl_multi_cleanup(ownedCurlMultiHandle) != CURLM_OK) {
            std::terminate();
        }
        ownedCurlMultiHandle = nullptr;
    }
    this->selfVoidPtr.reset();
    uvPollHandleMap.clear();
    uvTimerHandle.reset(nullptr);
    uvAsyncHandle.reset(nullptr);
}

JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient) {
    try {
        if (this_thread::get_id() != fetchClient.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient.jsContext) {
            JS_ThrowInternalError(jsContext, "FetchClient registration requires its live context");
            return nullptr;
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
                    // shutdown. The cleared map proves no work remains; do not
                    // touch the loop, async handle, or mailbox in that case.
                    if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
                        return;
                    }
                    // An earlier failure already requires host cleanup; do not
                    // schedule another settlement pass from this finalizer.
                    if (hasError(fetchClient->pendingError)) {
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
                        if (!hasError(fetchClient->pendingError)) {
                            *fetchClient->pendingError = std::error_code(status, uvErrorCategory);
                        }
                    }
                } catch (...) {
                    if (fetchClient == nullptr) {
                        std::terminate(); // No reporter means invalid callback storage.
                    }
                    if (!hasError(fetchClient->pendingError)) {
                        *fetchClient->pendingError = std::current_exception();
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
        OwnedJSValue client(jsContext, JS_NewObjectClass(jsContext, clientClassID));
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
        OwnedJSValue prototype(jsContext, JS_NewObject(jsContext));
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
