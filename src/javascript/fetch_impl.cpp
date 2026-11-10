#include "fetch_impl.hpp"

#include "javascript.hpp"
#include "void_ptr/void_ptr.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>
#include <curl/system.h>
#include <curl/urlapi.h>
#include <quickjs.h>
#include <uv.h>

#include <array>
#include <charconv>
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
#include <vector>

namespace amj::javascript {
namespace this_thread = std::this_thread;
namespace void_ptr = amj::void_ptr;

namespace {

std::mutex fetchResponseClassIDMutex = {};
std::map<JSRuntime*, JSClassID> fetchResponseClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

// Wake curl due to socket activity or timeout, then notify the loop to check
// for fetch results.
void wakeCurl(FetchClient& fetchClient, curl_socket_t socket, int eventBitmask) {
    // Have curl advance the transfers.
    int runningHandles = 0;
    const CURLMcode curlMultiCode = curl_multi_socket_action(fetchClient.ownedCurlMultiHandle, socket, eventBitmask, &runningHandles);
    if (curlMultiCode != CURLM_OK) {
        // If curl fails to advance the transfers, mark all fetches as failed
        // and unregister them from the curlMultiHandle.
        for (std::pair<CURL* const, FetchState>& entry : fetchClient.fetchStateMap) {
            CURL* curlEasyHandle = entry.first;
            FetchState& fetchState = entry.second;
            if (!fetchState.errorMessage.empty()) {
                fetchState.errorMessage.append(": ");
            }
            fetchState.errorMessage.append(curl_multi_strerror(curlMultiCode));
            fetchState.finished = true;
            if (fetchState.curlMultiHandle != nullptr) {
                const CURLMcode curlMultiCode = curl_multi_remove_handle(fetchState.curlMultiHandle, curlEasyHandle);
                if (curlMultiCode != CURLM_OK) {
                    std::terminate();
                }
                fetchState.curlMultiHandle = nullptr;
            }
        }
    }

    // Loop through all the completed transfers and mark their FetchState as
    // finished (optionally recording any error message).
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
            throw std::logic_error("Completed curl transfer has no FetchState");
        }
        FetchState& fetchState = fetchClient.fetchStateMap.at(curlMsg->easy_handle);
        fetchState.finished = true;
        if (curlMsg->data.result != CURLE_OK) {
            if (!fetchState.errorMessage.empty()) {
                fetchState.errorMessage.append(": ");
            }
            fetchState.errorMessage.append(curl_easy_strerror(curlMsg->data.result));
        }
        if (!fetchState.responseHeadersReceived) {
            if (!fetchState.errorMessage.empty()) {
                fetchState.errorMessage.append(": ");
            }
            fetchState.errorMessage.append("HTTP response ended without final headers");
        }
    }

    // Send a notification to check the fetchStateMap for results.
    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient.ownedUvAsyncHandle));
    const int status = uv_async_send(fetchClient.ownedUvAsyncHandle);
    if (status != 0) {
        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient.ownedUvAsyncHandle));
        throw std::runtime_error(uv_strerror(status));
    }
}

void settlePromises(uv_async_t* uvAsyncHandle) {
    if (uvAsyncHandle->data == nullptr) {
        return;
    }
    FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(uvAsyncHandle->data)->cast<FetchClient*>();
    uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle));

    std::vector<std::pair<CURL*, uint64_t>> entries;
    entries.reserve(fetchClient->fetchStateMap.size());
    for (const std::pair<CURL* const, FetchState>& entry : fetchClient->fetchStateMap) {
        entries.push_back({entry.first, entry.second.fetchStateID});
    }

    for (const std::pair<CURL*, uint64_t>& entry : entries) {
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
        // Curl may reuse CURL* pointers, so we need to check that the
        // fetchStateID matches the one that we previously snapshotted into
        // entries.
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
                throw std::logic_error("abandoned FetchState disappeared");
            }
            const FetchState& fetchState = mapNode.mapped();
            const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.readyPromise.promise.get());
            if (readyPromiseState != JS_PROMISE_PENDING && !fetchState.readPromise.has_value()) {
                continue;
            }
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "AbortError", "FetchResponse is closed");
            if (readyPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
            }
            if (fetchState.readPromise.has_value()) {
                const JSPromiseStateEnum readPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.readPromise.value().promise.get());
                if (readPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                    JS_FreeValue(fetchClient->jsContext, result);
                }
            }
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.readyPromise.promise.get());
        if (readyPromiseState != JS_PROMISE_FULFILLED && readyPromiseState != JS_PROMISE_REJECTED) {
            if (!fetchState.responseHeadersReceived) {
                if (!fetchState.finished) {
                    continue;
                }
                OwnedJSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.errorMessage);
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
                fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
                continue;
            }
            char* effectiveURL = nullptr; // NOLINT(misc-const-correctness): CURLINFO_EFFECTIVE_URL requires char**.
            const CURLcode err = curl_easy_getinfo(curlEasyHandle, CURLINFO_EFFECTIVE_URL, &effectiveURL);
            if (err != CURLE_OK || effectiveURL == nullptr) {
                fetchState.errorMessage = "Could not obtain response URL";
                OwnedJSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.errorMessage);
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
                fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
                continue;
            }
            fetchState.effectiveURL = effectiveURL;
            JSValue argument = JS_UNDEFINED;
            const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readyPromise.resolveFunction.get(), JS_UNDEFINED, 1, &argument);
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }

        if (!fetchState.readPromise.has_value()) {
            continue;
        }

        if (fetchState.errorMessage.empty() && fetchState.chunks.empty() && !fetchState.finished) {
            continue;
        }

        OwnedJSPromise read = std::move(fetchState.readPromise.value());
        fetchState.readPromise.reset();
        if (!fetchState.errorMessage.empty()) {
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "TypeError", fetchState.errorMessage);
            const JSValue result = JS_Call(fetchClient->jsContext, read.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }
        if (fetchState.chunks.empty()) {
            JSValue endOfStream = JS_NULL;
            const JSValue result = JS_Call(fetchClient->jsContext, read.resolveFunction.get(), JS_UNDEFINED, 1, &endOfStream);
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }
        const std::vector<uint8_t>& chunk = fetchState.chunks.front();
        OwnedJSValue bytes(fetchClient->jsContext, JS_NewUint8ArrayCopy(fetchClient->jsContext, chunk.data(), chunk.size()));
        fetchState.bufferedBytes -= chunk.size();
        fetchState.chunks.pop_front();
        if (fetchState.paused && fetchState.bufferedBytes <= (256U << 10U /* 256 KiB */) / 2 && !fetchState.finished) {
            fetchState.paused = false;
            const CURLcode status = curl_easy_pause(curlEasyHandle, CURLPAUSE_CONT);
            if (status != CURLE_OK) {
                fetchState.errorMessage = curl_easy_strerror(status);
                fetchState.finished = true;
                if (fetchState.curlMultiHandle != nullptr) {
                    if (curl_multi_remove_handle(fetchState.curlMultiHandle, curlEasyHandle) != CURLM_OK) {
                        std::terminate();
                    }
                    fetchState.curlMultiHandle = nullptr;
                }
            }
            // Unpause can synchronously invoke curl callbacks, but never JS.
            // Inspect completions even if no further socket event arrives.
            wakeCurl(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
        }
        if (JS_IsException(bytes.get())) {
            OwnedJSValue error(fetchClient->jsContext, JS_GetException(fetchClient->jsContext));
            const JSValue result = JS_Call(fetchClient->jsContext, read.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
            JS_FreeValue(fetchClient->jsContext, result);
            fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
            continue;
        }
        const JSValue result = JS_Call(fetchClient->jsContext, read.resolveFunction.get(), JS_UNDEFINED, 1, &bytes.get());
        JS_FreeValue(fetchClient->jsContext, result);
        fetchClient->jsExecutePendingJobs(fetchClient->jsContext);
    }
}

// Curl callbacks only mutate native state. Never invoke QuickJS from here.
// NOLINTNEXTLINE(misc-const-correctness): curl_write_callback requires char*.
std::size_t curlHeaderFunction(char* bytes, std::size_t size, std::size_t count, void* userdata) {
    FetchState* fetchState = static_cast<void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
    if (size != 0 && count > (64U << 10U /* 64 KiB */) / size) {
        fetchState->errorMessage = "Response headers exceed 64 KiB";
        return 0;
    }
    const std::size_t length = size * count;
    if (length > (64U << 10U /* 64 KiB */) - fetchState->headerBytes) {
        fetchState->errorMessage = "Response headers exceed 64 KiB";
        return 0;
    }
    fetchState->headerBytes += length;
    const std::string_view line(bytes, length);
    if (line.starts_with("HTTP/")) {
        fetchState->responseHeaders.clear();
        fetchState->redirecting = false;
        const std::size_t space = line.find(' ');
        if (space == std::string_view::npos || line.size() - space < 4) {
            fetchState->errorMessage = "Invalid HTTP status line";
            return 0;
        }
        const std::string_view code = line.substr(space + 1, 3);
        const std::from_chars_result result = std::from_chars(code.data(), code.data() + code.size(), fetchState->statusCode);
        if (result.ec != std::errc() || result.ptr != code.data() + code.size()) {
            fetchState->errorMessage = "Invalid HTTP status code";
            return 0;
        }
        return length;
    }
    if (line == "\r\n" || line == "\n") {
        if (fetchState->statusCode == 101) {
            fetchState->errorMessage = "HTTP protocol upgrades are not supported";
            return 0;
        }
        if (fetchState->statusCode >= 200 && !fetchState->redirecting) {
            fetchState->responseHeadersReceived = true;
        }
        return length;
    }
    if (fetchState->responseHeadersReceived) {
        return length; // Trailers are not part of the exposed header snapshot.
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        fetchState->errorMessage = "Invalid HTTP response header";
        return 0;
    }
    std::string name(line.substr(0, colon));
    for (char& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (name.find_first_not_of("!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyz") != std::string::npos) {
        fetchState->errorMessage = "Invalid HTTP response header name";
        return 0;
    }
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ((byte < 32 && byte != 9) || byte == 127) {
            fetchState->errorMessage = "Invalid HTTP response header value";
            return 0;
        }
    }
    if (name == "location" && !value.empty() && (fetchState->statusCode == 301 || fetchState->statusCode == 302 || fetchState->statusCode == 303 || fetchState->statusCode == 307 || fetchState->statusCode == 308)) {
        fetchState->redirecting = true;
    }
    fetchState->responseHeaders.push_back({name, std::string(value)});
    return length;
}

std::size_t curlWriteFunction(char* bytes, std::size_t size, std::size_t count, const void* userdata) {
    FetchState* fetchState = static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
    if (size != 0 && count > (256U << 10U /* 256 KiB */) / size) {
        fetchState->errorMessage = "HTTP body block exceeds buffer capacity";
        return 0;
    }
    const std::size_t length = size * count;
    if (fetchState->redirecting) {
        return length;
    }
    if (length == 0) {
        return 0;
    }
    if (length > (256U << 10U /* 256 KiB */) - fetchState->bufferedBytes) {
        fetchState->paused = true;
        return CURL_WRITEFUNC_PAUSE; // Enqueue nothing: curl retries this block.
    }
    // Curl supplies exactly length readable bytes; snapshot before returning.
    fetchState->chunks.push_back(std::vector<uint8_t>(bytes, bytes + length));
    fetchState->bufferedBytes += length;
    return length;
}

struct PollData {
    FetchClient* fetchClient = nullptr;
    curl_socket_t socket = CURL_SOCKET_BAD;
};

int curlSocketFunction(CURL* /* curlEasyHandle */, curl_socket_t socket, int action, const void* userdata, void* /* socketData */) {
    FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchClient*>();
    if (action == CURL_POLL_REMOVE) {
        const std::map<curl_socket_t, uv_poll_t*>::node_type socketNode = fetchClient->ownedUvPollHandleMap.extract(socket);
        if (!socketNode.empty()) {
            uv_close(reinterpret_cast<uv_handle_t*>(socketNode.mapped()), [](uv_handle_t* uvHandle) {
                delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
                delete reinterpret_cast<uv_poll_t*>(uvHandle);
            });
        }
        return 0;
    }
    if (!fetchClient->ownedUvPollHandleMap.contains(socket)) {
        std::unique_ptr<uv_poll_t> uvPollHandle = std::make_unique<uv_poll_t>();
        std::unique_ptr<PollData> pollData = std::make_unique<PollData>();
        pollData->fetchClient = fetchClient;
        pollData->socket = socket;
        std::unique_ptr<void_ptr::VoidPtr> voidPtr = std::make_unique<void_ptr::VoidPtr>(std::move(pollData));
        fetchClient->ownedUvPollHandleMap.insert_or_assign(socket, nullptr);
        const int status = uv_poll_init_socket(fetchClient->uvLoop, uvPollHandle.get(), socket);
        if (status != 0) {
            fetchClient->ownedUvPollHandleMap.erase(socket);
            return -1;
        }
        uvPollHandle->data = voidPtr.release();
        fetchClient->ownedUvPollHandleMap.at(socket) = uvPollHandle.release();
    }
    unsigned int events = 0;
    if (action == CURL_POLL_IN || action == CURL_POLL_INOUT) {
        events |= static_cast<unsigned int>(UV_READABLE);
    }
    if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
        events |= static_cast<unsigned int>(UV_WRITABLE);
    }
    uv_poll_t* uvPollHandle = fetchClient->ownedUvPollHandleMap.at(socket);
    if (events == 0) {
        return uv_poll_stop(uvPollHandle) == 0 ? 0 : -1;
    }
    const int status = uv_poll_start(uvPollHandle, static_cast<int>(events), [](uv_poll_t* uvPollHandle, int status, int events) noexcept {
        try {
            const PollData& pollData = static_cast<void_ptr::VoidPtr*>(uvPollHandle->data)->cast<PollData>();
            if (pollData.fetchClient == nullptr) {
                return;
            }
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
            // drive() may close this poll handle. Do not use pollData after it.
            wakeCurl(*pollData.fetchClient, pollData.socket, static_cast<int>(curlEvents));
        } catch (...) {
            std::terminate();
        }
    });
    return status == 0 ? 0 : -1;
}

int curlTimerFunction(CURLM* /* curlMultiHandle */, long milliseconds, const void* userdata) {
    const FetchClient* fetchClient = static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchClient*>();
    if (milliseconds < 0) {
        return uv_timer_stop(fetchClient->ownedUvTimerHandle) == 0 ? 0 : -1;
    }
    uv_update_time(fetchClient->uvLoop);
    const int status = uv_timer_start(fetchClient->ownedUvTimerHandle, [](uv_timer_t* uvTimerHandle) noexcept {
        try {
            FetchClient* fetchClient = static_cast<void_ptr::VoidPtr*>(uvTimerHandle->data)->cast<FetchClient*>();
            if (fetchClient != nullptr) {
                wakeCurl(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
            }
        } catch (...) {
            std::terminate();
        }
    },
        static_cast<uint64_t>(milliseconds), 0);
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
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.readyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
        }
        if (fetchState.readPromise.has_value()) {
            return JS_ThrowTypeError(jsContext, "Only one outstanding read is supported");
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        OwnedJSValue promise(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(promise.get())) {
            return JS_EXCEPTION;
        }
        fetchState.readPromise = OwnedJSPromise{
            .promise = std::move(promise),
            .resolveFunction = OwnedJSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = OwnedJSValue(jsContext, resolvingFunctions.at(1)),
        };
        uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient->ownedUvAsyncHandle));
        const int status = uv_async_send(fetchClient->ownedUvAsyncHandle);
        if (status != 0) {
            uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient->ownedUvAsyncHandle));
            throw std::runtime_error(uv_strerror(status));
        }
        return JS_DupValue(jsContext, fetchState.readPromise.value().promise.get());
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
        CURL* curlEasyHandle = fetchResponse.curlEasyHandle;
        fetchResponse.curlEasyHandle = nullptr;
        if (curlEasyHandle != nullptr) {
            const std::map<CURL*, FetchState>::node_type mapNode = fetchClient->fetchStateMap.extract(curlEasyHandle);
            if (mapNode.empty()) {
                return JS_UNDEFINED;
            }
            const FetchState& fetchState = mapNode.mapped();
            const JSPromiseStateEnum readyPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.readyPromise.promise.get());
            if (!fetchState.readPromise.has_value() && readyPromiseState != JS_PROMISE_PENDING) {
                return JS_UNDEFINED;
            }
            OwnedJSValue error = jsNewError(fetchClient->jsContext, "AbortError", "FetchResponse is closed");
            if (readyPromiseState == JS_PROMISE_PENDING) {
                const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(fetchClient->jsContext, result);
            }
            if (fetchState.readPromise.has_value()) {
                const JSPromiseStateEnum readPromiseState = JS_PromiseState(fetchClient->jsContext, fetchState.readPromise.value().promise.get());
                if (readPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(fetchClient->jsContext, fetchState.readPromise.value().rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
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
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.readyPromise.promise.get());
        if (promiseState != JS_PROMISE_FULFILLED && promiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
        }
        return JS_NewInt64(jsContext, fetchState.statusCode);
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
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.readyPromise.promise.get());
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
        const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, fetchState.readyPromise.promise.get());
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
        if (jsContext != fetchClient->jsContext) {
            return JS_ThrowInternalError(jsContext, "FetchClient belongs to another context");
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
        if (!JS_IsString(url.get()) || !JS_IsString(method.get()) || !JS_IsArray(headers.get()) || JS_GetTypedArrayType(body.get()) != JS_TYPED_ARRAY_UINT8) {
            return JS_ThrowTypeError(jsContext, "FetchRequest requires string url/method, header pairs, and Uint8Array body");
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
        if (byteOffset > bufferLength || byteLength > bufferLength - byteOffset || byteLength > (32U << 20U /* 32 MiB */)) {
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
        if (fetchClient->fetchStateMap.size() >= 64) {
            return JS_ThrowRangeError(jsContext, "Too many open FetchResponses");
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlEasyHandle(curl_easy_init(), curl_easy_cleanup);
        if (curlEasyHandle == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        fetchClient->fetchStateMap.try_emplace(curlEasyHandle.get());
        FetchState& fetchState = fetchClient->fetchStateMap.at(curlEasyHandle.get());
        fetchClient->fetchStateIDCounter++;
        fetchState.fetchStateID = fetchClient->fetchStateIDCounter;
        fetchState.fetchRequest = std::move(fetchRequest);
        registeredHandle = curlEasyHandle.get();
        fetchState.ownedCurlEasyHandle = curlEasyHandle.release();
        fetchState.voidPtr = std::make_unique<void_ptr::VoidPtr>(&fetchState);
        bool hasContentType = false;
        for (const std::pair<std::string, std::string>& header : fetchState.fetchRequest.headers) {
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
        CURLcode err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_URL, fetchState.fetchRequest.url.c_str());
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_PROTOCOLS_STR, "http,https");
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_FOLLOWLOCATION, CURLFOLLOW_OBEYCODE);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_MAXREDIRS, 20L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOSIGNAL, 1L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYPEER, 1L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYHOST, 2L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_ACCEPT_ENCODING, "");
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HTTPHEADER, fetchState.ownedCurlRequestHeaders);
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERDATA, fetchState.voidPtr.get());
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERFUNCTION, [](char* bytes, std::size_t size, std::size_t count, void* userdata) noexcept {
            try {
                return curlHeaderFunction(bytes, size, count, userdata);
            } catch (...) {
                return static_cast<std::size_t>(0);
            }
        });
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEDATA, fetchState.voidPtr.get());
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        err = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEFUNCTION, [](char* bytes, std::size_t size, std::size_t count, void* userdata) noexcept {
            try {
                return curlWriteFunction(bytes, size, count, userdata);
            } catch (...) {
                return static_cast<std::size_t>(0);
            }
        });
        if (err != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(err));
        }
        CURLcode methodStatus = CURLE_OK;
        if (fetchState.fetchRequest.method == "HEAD") {
            methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOBODY, 1L);
        } else if (fetchState.fetchRequest.method != "GET") {
            const char* bodyBytes = "";
            if (!fetchState.fetchRequest.body.empty()) {
                bodyBytes = reinterpret_cast<const char*>(fetchState.fetchRequest.body.data());
            }
            methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDS, bodyBytes);
            if (methodStatus == CURLE_OK) {
                methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(fetchState.fetchRequest.body.size()));
            }
            if (methodStatus == CURLE_OK && fetchState.fetchRequest.method != "POST") {
                methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_CUSTOMREQUEST, fetchState.fetchRequest.method.c_str());
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
        fetchState.readyPromise = OwnedJSPromise{
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
        if (JS_DefinePropertyValueStr(jsContext, response.get(), "ready", JS_DupValue(jsContext, fetchState.readyPromise.promise.get()), JS_PROP_ENUMERABLE) < 0) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_EXCEPTION;
        }
        const CURLMcode curlMultiCode = curl_multi_add_handle(
            fetchClient->ownedCurlMultiHandle,
            fetchState.ownedCurlEasyHandle
        );
        if (curlMultiCode != CURLM_OK) {
            fetchClient->fetchStateMap.erase(registeredHandle);
            return JS_ThrowInternalError(jsContext, "curl_multi_add_handle: %s", curl_multi_strerror(curlMultiCode));
        }
        fetchState.curlMultiHandle = fetchClient->ownedCurlMultiHandle;
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

FetchClient::FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> jsExecutePendingJobs) {
    if (jsContext == nullptr || uvLoop == nullptr || !jsExecutePendingJobs) {
        throw std::invalid_argument("FetchClient requires a context, initialized loop, and completion hook");
    }
    this->jsRuntime = JS_GetRuntime(jsContext);
    this->jsContext = jsContext;
    this->uvLoop = uvLoop;
    this->jsExecutePendingJobs = std::move(jsExecutePendingJobs);
    this->ownerThreadID = this_thread::get_id();
    try {
        static std::once_flag curlInitialization;
        std::call_once(curlInitialization, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                throw std::runtime_error("curl_global_init failed");
            }
        });
        this->voidPtr = std::make_unique<void_ptr::VoidPtr>(this);
        ownedCurlMultiHandle = curl_multi_init();
        if (ownedCurlMultiHandle == nullptr) {
            throw std::bad_alloc();
        }
        std::unique_ptr<void_ptr::VoidPtr> voidPtr;
        {
            std::unique_ptr<uv_timer_t> uvTimerHandle = std::make_unique<uv_timer_t>();
            voidPtr = std::make_unique<void_ptr::VoidPtr>(this);
            const int status = uv_timer_init(uvLoop, uvTimerHandle.get());
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvTimerHandle->data = voidPtr.release();
            ownedUvTimerHandle = uvTimerHandle.release();
        }
        {
            std::unique_ptr<uv_async_t> uvAsyncHandle = std::make_unique<uv_async_t>();
            voidPtr = std::make_unique<void_ptr::VoidPtr>(this);
            const int status = uv_async_init(uvLoop, uvAsyncHandle.get(), [](uv_async_t* uvAsyncHandle) noexcept {
                try {
                    settlePromises(uvAsyncHandle);
                } catch (...) {
                    std::terminate();
                }
            });
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvAsyncHandle->data = voidPtr.release();
            ownedUvAsyncHandle = uvAsyncHandle.release();
            uv_unref(reinterpret_cast<uv_handle_t*>(ownedUvAsyncHandle));
        }
        CURLMcode err = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_SOCKETFUNCTION, [](CURL* curlEasyHandle, curl_socket_t socket, int action, void* userdata, void* socketData) noexcept {
            try {
                return curlSocketFunction(curlEasyHandle, socket, action, userdata, socketData);
            } catch (...) {
                return -1;
            }
        });
        if (err != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(err));
        }
        err = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_SOCKETDATA, this->voidPtr.get());
        if (err != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(err));
        }
        err = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_TIMERFUNCTION, [](CURLM* curlMultiHandle, long milliseconds, void* userdata) noexcept {
            try {
                return curlTimerFunction(curlMultiHandle, milliseconds, userdata);
            } catch (...) {
                return -1;
            }
        });
        if (err != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(err));
        }
        err = curl_multi_setopt(ownedCurlMultiHandle, CURLMOPT_TIMERDATA, this->voidPtr.get());
        if (err != CURLM_OK) {
            throw std::runtime_error(curl_multi_strerror(err));
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
    this->voidPtr.reset();
    for (const std::pair<const curl_socket_t, uv_poll_t*>& entry : ownedUvPollHandleMap) {
        uv_close(reinterpret_cast<uv_handle_t*>(entry.second), [](uv_handle_t* uvHandle) {
            delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
            delete reinterpret_cast<uv_poll_t*>(uvHandle);
        });
    }
    ownedUvPollHandleMap.clear();
    if (ownedUvTimerHandle != nullptr) {
        uv_close(reinterpret_cast<uv_handle_t*>(ownedUvTimerHandle), [](uv_handle_t* uvHandle) {
            delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
            delete reinterpret_cast<uv_timer_t*>(uvHandle);
        });
        ownedUvTimerHandle = nullptr;
    }
    if (ownedUvAsyncHandle != nullptr) {
        delete static_cast<void_ptr::VoidPtr*>(ownedUvAsyncHandle->data);
        ownedUvAsyncHandle->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(ownedUvAsyncHandle), [](uv_handle_t* uvHandle) {
            delete reinterpret_cast<uv_async_t*>(uvHandle);
        });
        ownedUvAsyncHandle = nullptr;
    }
}

JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient) {
    try {
        if (this_thread::get_id() != fetchClient.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient.jsContext || fetchClient.shuttingDown) {
            JS_ThrowInternalError(jsContext, "FetchClient registration requires its live context and an active client");
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
                try {
                    const std::unique_ptr<void_ptr::VoidPtr> voidPtr(static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value, JS_GetClassID(value))));
                    if (voidPtr == nullptr) {
                        return;
                    }
                    const FetchResponse& fetchResponse = voidPtr->cast<FetchResponse>();
                    if (fetchResponse.curlEasyHandle == nullptr) {
                        return;
                    }
                    FetchClient* fetchClient = fetchResponse.fetchClient;
                    if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
                        return;
                    }
                    // GC can run while allocating promise results. Do
                    // not erase a FetchState currently being accessed
                    // by settlePromises(), and do not settle promises
                    // from inside a QuickJS finalizer.
                    fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle).abandoned = true;
                    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient->ownedUvAsyncHandle));
                    const int status = uv_async_send(fetchClient->ownedUvAsyncHandle);
                    if (status != 0) {
                        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient->ownedUvAsyncHandle));
                        throw std::runtime_error(uv_strerror(status));
                    }
                } catch (...) {
                    std::terminate();
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
