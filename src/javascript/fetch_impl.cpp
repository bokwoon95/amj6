#include "fetch_impl.hpp"

#include "javascript.hpp"

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

namespace {

struct PollData {
    FetchClient* fetchClient = nullptr;
    curl_socket_t socket = CURL_SOCKET_BAD;
};

void closeTimerHandle(uv_timer_t* uvTimerHandle) {
    if (uvTimerHandle == nullptr) {
        return;
    }
    uvTimerHandle->data = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(uvTimerHandle), [](uv_handle_t* uvHandle) {
        const uv_timer_t* uvTimerHandle = reinterpret_cast<uv_timer_t*>(uvHandle);
        delete uvTimerHandle;
    });
}

void closePollHandle(uv_poll_t* uvPollHandle) {
    PollData* pollData = static_cast<PollData*>(uvPollHandle->data);
    pollData->fetchClient = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(uvPollHandle), [](uv_handle_t* uvHandle) {
        const uv_poll_t* uvPollHandle = reinterpret_cast<uv_poll_t*>(uvHandle);
        delete static_cast<PollData*>(uvPollHandle->data);
        delete uvPollHandle;
    });
}

JSValue newFetchError(JSContext* jsContext, const char* name, std::string_view message) {
    ScopedJSValue error(jsContext, JS_NewError(jsContext));
    if (JS_IsException(error.get())) {
        return JS_EXCEPTION;
    }
    if (!setstringfield(jsContext, error.get(), "name", name) || !setstringfield(jsContext, error.get(), "message", message)) {
        return JS_EXCEPTION;
    }
    return error.release();
}

void closeResponse(FetchClient& fetchClient, CURL* curlEasyHandle, bool settle) {
    JSContext* jsContext = fetchClient.jsContext;
    const std::map<CURL*, FetchState>::node_type stateNode = fetchClient.fetchStates.extract(curlEasyHandle);
    if (stateNode.empty()) {
        return;
    }
    FetchState& fetchState = stateNode.mapped();
    if (fetchState.addedToMulti) {
        const CURLMcode status = curl_multi_remove_handle(fetchClient.curlMultiHandle, curlEasyHandle);
        if (status != CURLM_OK) {
            std::terminate(); // Never destroy callback storage still owned by curl.
        }
        fetchState.addedToMulti = false;
    }
    if (!settle || (JS_IsUndefined(fetchState.readyReject) && JS_IsUndefined(fetchState.readReject))) {
        return;
    }
    JSValue argument = newFetchError(jsContext, "AbortError", "FetchResponse is closed");
    if (JS_IsException(argument)) {
        argument = JS_GetException(jsContext);
    }
    const ScopedJSValue reason(jsContext, argument);
    // Resolve/reject functions are native Promise functions, not user callbacks.
    // Promise continuations run at the host's next job checkpoint.
    for (const JSValue reject : {fetchState.readyReject, fetchState.readReject}) {
        if (!JS_IsUndefined(reject)) {
            const ScopedJSValue result(jsContext, JS_Call(jsContext, reject, JS_UNDEFINED, 1, &argument));
        }
    }
}

void scheduleDelivery(const FetchClient& fetchClient) {
    if (fetchClient.shuttingDown) {
        return;
    }
    uv_ref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle));
    const int status = uv_async_send(fetchClient.uvAsyncHandle);
    if (status != 0) {
        uv_unref(reinterpret_cast<uv_handle_t*>(fetchClient.uvAsyncHandle));
        throw std::runtime_error(uv_strerror(status));
    }
}

void drive(FetchClient& fetchClient, curl_socket_t socket, int events) {
    if (fetchClient.shuttingDown) {
        return;
    }
    int running = 0;
    const CURLMcode status = curl_multi_socket_action(fetchClient.curlMultiHandle, socket, events, &running);
    if (status != CURLM_OK) {
        for (std::pair<CURL* const, FetchState>& entry : fetchClient.fetchStates) {
            FetchState& fetchState = entry.second;
            if (fetchState.failure.empty()) {
                fetchState.failure = curl_multi_strerror(status);
            }
            fetchState.finished = true;
            if (fetchState.addedToMulti) {
                if (curl_multi_remove_handle(fetchClient.curlMultiHandle, entry.first) != CURLM_OK) {
                    std::terminate();
                }
                fetchState.addedToMulti = false;
            }
        }
    }
    int remaining = 0;
    while (const CURLMsg* message = curl_multi_info_read(fetchClient.curlMultiHandle, &remaining)) {
        if (message->msg != CURLMSG_DONE || !fetchClient.fetchStates.contains(message->easy_handle)) {
            continue;
        }
        FetchState& fetchState = fetchClient.fetchStates.at(message->easy_handle);
        fetchState.finished = true;
        if (message->data.result != CURLE_OK && fetchState.failure.empty()) {
            fetchState.failure = curl_easy_strerror(message->data.result);
        }
        if (!fetchState.headersReceived && fetchState.failure.empty()) {
            fetchState.failure = "HTTP response ended without final headers";
        }
        const CURLMcode removeStatus = curl_multi_remove_handle(fetchClient.curlMultiHandle, message->easy_handle);
        if (removeStatus != CURLM_OK) {
            std::terminate();
        }
        fetchState.addedToMulti = false;
    }
    scheduleDelivery(fetchClient);
}

void deliver(FetchClient& fetchClient) {
    JSContext* jsContext = fetchClient.jsContext;
    // One completion per response per turn. A fast stream must not monopolize
    // delivery or keep libuv from running timers and other socket callbacks.
    std::vector<CURL*> responseKeys;
    responseKeys.reserve(fetchClient.fetchStates.size());
    for (const std::pair<CURL* const, FetchState>& entry : fetchClient.fetchStates) {
        responseKeys.push_back(entry.first);
    }
    bool didDeliver = false;
    for (CURL* selected : responseKeys) {
        if (fetchClient.shuttingDown) {
            return;
        }
        if (!fetchClient.fetchStates.contains(selected)) {
            continue;
        }
        bool deliverReady = false;
        bool shouldCloseResponse = false;
        {
            const FetchState& fetchState = fetchClient.fetchStates.at(selected);
            if (fetchState.closeRequested) {
                shouldCloseResponse = true;
            } else if (!fetchState.readyDelivered && (fetchState.headersReceived || fetchState.finished)) {
                deliverReady = true;
            } else if (JS_IsUndefined(fetchState.readResolve) || (fetchState.failure.empty() && fetchState.chunks.empty() && !fetchState.finished)) {
                continue;
            }
        }
        didDeliver = true;
        if (shouldCloseResponse) {
            closeResponse(fetchClient, selected, true);
            fetchClient.callbackDidRun(jsContext);
            continue;
        }
        {
            FetchState& fetchState = fetchClient.fetchStates.at(selected);
            JSValue resolveValue = JS_UNDEFINED;
            JSValue rejectValue = JS_UNDEFINED;
            JSValue argumentValue = JS_UNDEFINED;
            bool failed = false;
            if (deliverReady) {
                fetchState.readyDelivered = true;
                resolveValue = std::exchange(fetchState.readyResolve, JS_UNDEFINED);
                rejectValue = std::exchange(fetchState.readyReject, JS_UNDEFINED);
                if (fetchState.headersReceived) {
                    char* effectiveURL = nullptr; // NOLINT(misc-const-correctness): CURLINFO_EFFECTIVE_URL requires char**.
                    const CURLcode status = curl_easy_getinfo(selected, CURLINFO_EFFECTIVE_URL, &effectiveURL);
                    if (status != CURLE_OK || effectiveURL == nullptr) {
                        fetchState.failure = "Could not obtain response URL";
                        failed = true;
                    } else {
                        fetchState.responseURL = effectiveURL;
                    }
                } else {
                    failed = true;
                }
            } else {
                resolveValue = std::exchange(fetchState.readResolve, JS_UNDEFINED);
                rejectValue = std::exchange(fetchState.readReject, JS_UNDEFINED);
                if (!fetchState.failure.empty()) {
                    failed = true;
                } else if (!fetchState.chunks.empty()) {
                    const std::vector<uint8_t>& chunk = fetchState.chunks.front();
                    argumentValue = JS_NewUint8ArrayCopy(jsContext, chunk.data(), chunk.size());
                    fetchState.bufferedBytes -= chunk.size();
                    fetchState.chunks.pop_front();
                    if (fetchState.paused && fetchState.bufferedBytes <= (256U << 10U /* 256 KiB */) / 2 && !fetchState.finished) {
                        fetchState.paused = false;
                        const CURLcode status = curl_easy_pause(selected, CURLPAUSE_CONT);
                        if (status != CURLE_OK) {
                            fetchState.failure = curl_easy_strerror(status);
                            fetchState.finished = true;
                            if (fetchState.addedToMulti) {
                                if (curl_multi_remove_handle(fetchClient.curlMultiHandle, selected) != CURLM_OK) {
                                    std::terminate();
                                }
                                fetchState.addedToMulti = false;
                            }
                        }
                        // Unpause can synchronously invoke curl callbacks, but never JS.
                        // Inspect completions even if no further socket event arrives.
                        drive(fetchClient, CURL_SOCKET_TIMEOUT, 0);
                    }
                } else {
                    argumentValue = JS_NULL;
                }
            }
            const ScopedJSValue resolve(jsContext, resolveValue);
            const ScopedJSValue reject(jsContext, rejectValue);
            if (failed) {
                argumentValue = newFetchError(jsContext, "TypeError", fetchState.failure);
            }
            if (JS_IsException(argumentValue)) {
                argumentValue = JS_GetException(jsContext);
                failed = true;
            }
            const ScopedJSValue argument(jsContext, argumentValue);
            const JSValue callback = failed ? reject.get() : resolve.get();
            const ScopedJSValue result(jsContext, JS_Call(jsContext, callback, JS_UNDEFINED, 1, &argumentValue));
        }
        // The checkpoint can close responses or shut down the whole client.
        // No map references or iterators survive across it.
        fetchClient.callbackDidRun(jsContext);
    }
    if (didDeliver && !fetchClient.shuttingDown) {
        scheduleDelivery(fetchClient);
    }
}

// Curl callbacks only mutate native state. Never invoke QuickJS from here.
// NOLINTNEXTLINE(misc-const-correctness): curl_write_callback requires char*.
std::size_t receiveHeader(char* bytes, std::size_t size, std::size_t count, void* userdata) noexcept {
    try {
        FetchState* fetchState = static_cast<FetchState*>(userdata);
        if (size != 0 && count > (64U << 10U /* 64 KiB */) / size) {
            fetchState->failure = "Response headers exceed 64 KiB";
            return 0;
        }
        const std::size_t length = size * count;
        if (length > (64U << 10U /* 64 KiB */) - fetchState->headerBytes) {
            fetchState->failure = "Response headers exceed 64 KiB";
            return 0;
        }
        fetchState->headerBytes += length;
        const std::string_view line(bytes, length);
        if (line.starts_with("HTTP/")) {
            fetchState->responseHeaders.clear();
            fetchState->redirecting = false;
            const std::size_t space = line.find(' ');
            if (space == std::string_view::npos || line.size() - space < 4) {
                fetchState->failure = "Invalid HTTP status line";
                return 0;
            }
            const std::string_view code = line.substr(space + 1, 3);
            const std::from_chars_result result = std::from_chars(code.data(), code.data() + code.size(), fetchState->statusCode);
            if (result.ec != std::errc() || result.ptr != code.data() + code.size()) {
                fetchState->failure = "Invalid HTTP status code";
                return 0;
            }
            return length;
        }
        if (line == "\r\n" || line == "\n") {
            if (fetchState->statusCode == 101) {
                fetchState->failure = "HTTP protocol upgrades are not supported";
                return 0;
            }
            if (fetchState->statusCode >= 200 && !fetchState->redirecting) {
                fetchState->headersReceived = true;
            }
            return length;
        }
        if (fetchState->headersReceived) {
            return length; // Trailers are not part of the exposed header snapshot.
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            fetchState->failure = "Invalid HTTP response header";
            return 0;
        }
        std::string name(line.substr(0, colon));
        for (char& character : name) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        if (name.find_first_not_of("!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyz") != std::string::npos) {
            fetchState->failure = "Invalid HTTP response header name";
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
                fetchState->failure = "Invalid HTTP response header value";
                return 0;
            }
        }
        if (name == "location" && !value.empty() && (fetchState->statusCode == 301 || fetchState->statusCode == 302 || fetchState->statusCode == 303 || fetchState->statusCode == 307 || fetchState->statusCode == 308)) {
            fetchState->redirecting = true;
        }
        fetchState->responseHeaders.push_back({name, std::string(value)});
        return length;
    } catch (...) {
        return 0; // Curl reports callback failure; no exception crosses its C ABI.
    }
}

// NOLINTNEXTLINE(misc-const-correctness): curl_write_callback requires char*.
std::size_t receiveBody(char* bytes, std::size_t size, std::size_t count, void* userdata) noexcept {
    try {
        FetchState* fetchState = static_cast<FetchState*>(userdata);
        if (size != 0 && count > (256U << 10U /* 256 KiB */) / size) {
            fetchState->failure = "HTTP body block exceeds buffer capacity";
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
            return CURL_WRITEFUNC_PAUSE; // Enqueue nothing: curl redelivers this block.
        }
        // Curl supplies exactly length readable bytes; snapshot before returning.
        fetchState->chunks.push_back(std::vector<uint8_t>(bytes, bytes + length));
        fetchState->bufferedBytes += length;
        return length;
    } catch (...) {
        return 0;
    }
}

int socketInterest(CURL* /* curlEasyHandle */, curl_socket_t socket, int action, void* userdata, void* /* socketData */) noexcept {
    try {
        FetchClient* fetchClient = static_cast<FetchClient*>(userdata);
        if (action == CURL_POLL_REMOVE) {
            const std::map<curl_socket_t, uv_poll_t*>::node_type socketNode = fetchClient->socketMap.extract(socket);
            if (!socketNode.empty()) {
                closePollHandle(socketNode.mapped());
            }
            return 0;
        }
        if (fetchClient->shuttingDown) {
            return 0;
        }
        if (!fetchClient->socketMap.contains(socket)) {
            std::unique_ptr<uv_poll_t> uvPollHandle = std::make_unique<uv_poll_t>();
            std::unique_ptr<PollData> pollData = std::make_unique<PollData>();
            pollData->fetchClient = fetchClient;
            pollData->socket = socket;
            fetchClient->socketMap.insert_or_assign(socket, nullptr);
            const int status = uv_poll_init_socket(fetchClient->uvLoop, uvPollHandle.get(), socket);
            if (status != 0) {
                fetchClient->socketMap.erase(socket);
                return -1;
            }
            uvPollHandle->data = pollData.release();
            fetchClient->socketMap.at(socket) = uvPollHandle.release();
        }
        unsigned int events = 0;
        if (action == CURL_POLL_IN || action == CURL_POLL_INOUT) {
            events |= UV_READABLE;
        }
        if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
            events |= UV_WRITABLE;
        }
        uv_poll_t* uvPollHandle = fetchClient->socketMap.at(socket);
        if (events == 0) {
            return uv_poll_stop(uvPollHandle) == 0 ? 0 : -1;
        }
        const int status = uv_poll_start(uvPollHandle, static_cast<int>(events), [](uv_poll_t* uvPollHandle, int status, int events) noexcept {
            try {
                const PollData* pollData = static_cast<PollData*>(uvPollHandle->data);
                if (pollData->fetchClient == nullptr) {
                    return;
                }
                unsigned int curlEvents = 0;
                if (status < 0) {
                    curlEvents |= CURL_CSELECT_ERR;
                }
                if ((static_cast<unsigned int>(events) & UV_READABLE) != 0) {
                    curlEvents |= CURL_CSELECT_IN;
                }
                if ((static_cast<unsigned int>(events) & UV_WRITABLE) != 0) {
                    curlEvents |= CURL_CSELECT_OUT;
                }
                // drive() may close this poll handle. Do not use pollData after it.
                drive(*pollData->fetchClient, pollData->socket, static_cast<int>(curlEvents));
            } catch (...) {
                std::terminate();
            }
        });
        return status == 0 ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

int timerInterest(CURLM* /* curlMultiHandle */, long milliseconds, void* userdata) noexcept {
    try {
        const FetchClient* fetchClient = static_cast<FetchClient*>(userdata);
        if (fetchClient->shuttingDown) {
            return 0;
        }
        if (milliseconds < 0) {
            return uv_timer_stop(fetchClient->uvTimeoutHandle) == 0 ? 0 : -1;
        }
        uv_update_time(fetchClient->uvLoop);
        const int status = uv_timer_start(fetchClient->uvTimeoutHandle, [](uv_timer_t* uvTimerHandle) noexcept {
            try {
                FetchClient* fetchClient = static_cast<FetchClient*>(uvTimerHandle->data);
                if (fetchClient != nullptr) {
                    drive(*fetchClient, CURL_SOCKET_TIMEOUT, 0);
                }
            } catch (...) {
                std::terminate();
            }
        },
            static_cast<uint64_t>(milliseconds), 0);
        return status == 0 ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

} // namespace

JSValue FetchResponse::read(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* functionData) {
    try {
        FetchClient* fetchClient = static_cast<FetchClient*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
        const FetchResponse* fetchResponse = static_cast<FetchResponse*>(JS_GetOpaque2(jsContext, thisValue, fetchClient->responseClassID));
        if (fetchResponse == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchResponse->fetchClient != fetchClient || jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchClient->shuttingDown || fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStates.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        FetchState& fetchState = fetchClient->fetchStates.at(fetchResponse->curlEasyHandle);
        if (!fetchState.readyDelivered) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
        }
        if (!JS_IsUndefined(fetchState.readResolve)) {
            return JS_ThrowTypeError(jsContext, "Only one outstanding read is supported");
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        ScopedJSValue promise(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(promise.get())) {
            return JS_EXCEPTION;
        }
        fetchState.readResolve = resolvingFunctions.at(0);
        fetchState.readReject = resolvingFunctions.at(1);
        scheduleDelivery(*fetchClient);
        return promise.release();
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: unexpected native failure");
    }
}

JSValue FetchResponse::close(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* functionData) {
    try {
        FetchClient* fetchClient = static_cast<FetchClient*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
        FetchResponse* fetchResponse = static_cast<FetchResponse*>(JS_GetOpaque2(jsContext, thisValue, fetchClient->responseClassID));
        if (fetchResponse == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchResponse->fetchClient != fetchClient || jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        CURL* curlEasyHandle = fetchResponse->curlEasyHandle;
        fetchResponse->curlEasyHandle = nullptr;
        if (!fetchClient->shuttingDown && curlEasyHandle != nullptr) {
            closeResponse(*fetchClient, curlEasyHandle, true);
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

JSValue FetchResponse::getStatus(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* functionData) {
    try {
        const FetchClient* fetchClient = static_cast<FetchClient*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
        const FetchResponse* fetchResponse = static_cast<FetchResponse*>(JS_GetOpaque2(jsContext, thisValue, fetchClient->responseClassID));
        if (fetchResponse == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchResponse->fetchClient != fetchClient || jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchClient->shuttingDown || fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStates.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStates.at(fetchResponse->curlEasyHandle);
        if (!fetchState.readyDelivered) {
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

JSValue FetchResponse::getHeaders(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* functionData) {
    try {
        const FetchClient* fetchClient = static_cast<FetchClient*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
        const FetchResponse* fetchResponse = static_cast<FetchResponse*>(JS_GetOpaque2(jsContext, thisValue, fetchClient->responseClassID));
        if (fetchResponse == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchResponse->fetchClient != fetchClient || jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchClient->shuttingDown || fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStates.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStates.at(fetchResponse->curlEasyHandle);
        if (!fetchState.readyDelivered) {
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

JSValue FetchResponse::getURL(JSContext* jsContext, JSValueConst thisValue, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* functionData) {
    try {
        const FetchClient* fetchClient = static_cast<FetchClient*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
        const FetchResponse* fetchResponse = static_cast<FetchResponse*>(JS_GetOpaque2(jsContext, thisValue, fetchClient->responseClassID));
        if (fetchResponse == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchResponse->fetchClient != fetchClient || jsContext != fetchClient->jsContext) {
            return JS_ThrowTypeError(jsContext, "Invalid FetchResponse context");
        }
        if (fetchClient->shuttingDown || fetchResponse->curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse is closed");
        }
        if (!fetchClient->fetchStates.contains(fetchResponse->curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext, "FetchResponse state is missing");
        }
        const FetchState& fetchState = fetchClient->fetchStates.at(fetchResponse->curlEasyHandle);
        if (!fetchState.readyDelivered) {
            return JS_ThrowTypeError(jsContext, "Await FetchResponse.ready before accessing the response");
        }
        return JS_NewStringLen(jsContext, fetchState.responseURL.data(), fetchState.responseURL.size());
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
        fetchClient = static_cast<FetchClient*>(JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0])));
        if (fetchClient == nullptr) {
            return JS_EXCEPTION;
        }
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient->jsContext || fetchClient->shuttingDown) {
            return JS_ThrowInternalError(jsContext, "FetchClient is shut down or belongs to another context");
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(jsContext, "send requires a FetchRequest object");
        }
        FetchRequest fetchRequest;
        const ScopedJSValue url(jsContext, JS_GetPropertyStr(jsContext, argv[0], "url"));
        const ScopedJSValue method(jsContext, JS_GetPropertyStr(jsContext, argv[0], "method"));
        const ScopedJSValue headers(jsContext, JS_GetPropertyStr(jsContext, argv[0], "headers"));
        const ScopedJSValue body(jsContext, JS_GetPropertyStr(jsContext, argv[0], "body"));
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
        const ScopedJSValue headerCountValue(jsContext, JS_GetPropertyStr(jsContext, headers.get(), "length"));
        uint32_t headerCount = 0;
        if (JS_IsException(headerCountValue.get()) || JS_ToUint32(jsContext, &headerCount, headerCountValue.get()) < 0) {
            return JS_EXCEPTION;
        }
        if (headerCount > (64U << 10U /* 64 KiB */) / 4) {
            return JS_ThrowRangeError(jsContext, "Too many request headers");
        }
        std::size_t headerBytes = 0;
        for (uint32_t index = 0; index < headerCount; index++) {
            const ScopedJSValue pair(jsContext, JS_GetPropertyUint32(jsContext, headers.get(), index));
            if (JS_IsException(pair.get())) {
                return JS_EXCEPTION;
            }
            if (!JS_IsArray(pair.get())) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const ScopedJSValue pairLengthValue(jsContext, JS_GetPropertyStr(jsContext, pair.get(), "length"));
            uint32_t pairLength = 0;
            if (JS_IsException(pairLengthValue.get()) || JS_ToUint32(jsContext, &pairLength, pairLengthValue.get()) < 0) {
                return JS_EXCEPTION;
            }
            if (pairLength != 2) {
                return JS_ThrowTypeError(jsContext, "Headers must be two-string arrays");
            }
            const ScopedJSValue name(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 0));
            const ScopedJSValue value(jsContext, JS_GetPropertyUint32(jsContext, pair.get(), 1));
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
        const ScopedJSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, body.get(), &byteOffset, &byteLength, nullptr));
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
        // Property getters above can reenter JS and shut down the client.
        if (fetchClient->shuttingDown) {
            return JS_ThrowInternalError(jsContext, "FetchClient is shut down");
        }
        if (fetchClient->fetchStates.size() >= 64) {
            return JS_ThrowRangeError(jsContext, "Too many open FetchResponses");
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlEasyHandle(curl_easy_init(), curl_easy_cleanup);
        if (curlEasyHandle == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        fetchClient->fetchStates.try_emplace(curlEasyHandle.get());
        FetchState& fetchState = fetchClient->fetchStates.at(curlEasyHandle.get());
        fetchState.jsRuntime = fetchClient->jsRuntime;
        fetchState.fetchRequest = std::move(fetchRequest);
        registeredHandle = curlEasyHandle.get();
        fetchState.curlEasyHandle = curlEasyHandle.release();
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
            curl_slist* curlHeaders = curl_slist_append(fetchState.curlRequestHeaders, line.c_str());
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.curlRequestHeaders = curlHeaders;
        }
        if (!hasContentType) {
            // POSTFIELDS otherwise invents application/x-www-form-urlencoded.
            // The JS facade supplies a content type for strings, but not bytes.
            curl_slist* curlHeaders = curl_slist_append(fetchState.curlRequestHeaders, "Content-Type:");
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.curlRequestHeaders = curlHeaders;
        }
        CURL* easy = fetchState.curlEasyHandle;
        const std::vector<CURLcode> optionResults = {
            curl_easy_setopt(easy, CURLOPT_URL, fetchState.fetchRequest.url.c_str()),
            curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https"),
            curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https"),
            curl_easy_setopt(easy, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L),
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, CURLFOLLOW_OBEYCODE),
            curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 20L),
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L),
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L),
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L),
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L),
            curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L),
            curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, ""),
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, fetchState.curlRequestHeaders),
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, receiveHeader),
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, &fetchState),
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receiveBody),
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fetchState),
        };
        for (const CURLcode result : optionResults) {
            if (result != CURLE_OK) {
                throw std::runtime_error(curl_easy_strerror(result));
            }
        }
        CURLcode methodStatus = CURLE_OK;
        if (fetchState.fetchRequest.method == "HEAD") {
            methodStatus = curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        } else if (fetchState.fetchRequest.method != "GET") {
            const char* bodyBytes = "";
            if (!fetchState.fetchRequest.body.empty()) {
                bodyBytes = reinterpret_cast<const char*>(fetchState.fetchRequest.body.data());
            }
            methodStatus = curl_easy_setopt(easy, CURLOPT_POSTFIELDS, bodyBytes);
            if (methodStatus == CURLE_OK) {
                methodStatus = curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(fetchState.fetchRequest.body.size()));
            }
            if (methodStatus == CURLE_OK && fetchState.fetchRequest.method != "POST") {
                methodStatus = curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, fetchState.fetchRequest.method.c_str());
            }
        }
        if (methodStatus != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(methodStatus));
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        ScopedJSValue ready(jsContext, JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(ready.get())) {
            closeResponse(*fetchClient, registeredHandle, false);
            return JS_EXCEPTION;
        }
        fetchState.readyResolve = resolvingFunctions.at(0);
        fetchState.readyReject = resolvingFunctions.at(1);
        ScopedJSValue response(jsContext, JS_NewObjectClass(jsContext, fetchClient->responseClassID));
        if (JS_IsException(response.get())) {
            closeResponse(*fetchClient, registeredHandle, false);
            return JS_EXCEPTION;
        }
        std::unique_ptr<FetchResponse> fetchResponse = std::make_unique<FetchResponse>();
        fetchResponse->fetchClient = fetchClient;
        fetchResponse->curlEasyHandle = registeredHandle;
        FetchResponse* responsePointer = fetchResponse.release();
        if (JS_SetOpaque(response.get(), responsePointer) < 0) {
            delete responsePointer;
            closeResponse(*fetchClient, registeredHandle, false);
            return JS_ThrowInternalError(jsContext, "Could not attach FetchResponse");
        }
        if (JS_DefinePropertyValueStr(jsContext, response.get(), "ready", ready.release(), JS_PROP_ENUMERABLE) < 0) {
            return JS_EXCEPTION; // Response finalization closes the registered state.
        }
        const CURLMcode addStatus = curl_multi_add_handle(fetchClient->curlMultiHandle, easy);
        if (addStatus != CURLM_OK) {
            return JS_ThrowInternalError(jsContext, "curl_multi_add_handle: %s", curl_multi_strerror(addStatus));
        }
        fetchState.addedToMulti = true;
        return response.release();
    } catch (const std::bad_alloc&) {
        if (registeredHandle != nullptr) {
            closeResponse(*fetchClient, registeredHandle, false);
        }
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (registeredHandle != nullptr) {
            closeResponse(*fetchClient, registeredHandle, false);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: %s", exception.what());
    } catch (...) {
        if (registeredHandle != nullptr) {
            closeResponse(*fetchClient, registeredHandle, false);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: unexpected native failure");
    }
}

FetchState::~FetchState() {
    // The client removes this easy handle from the multi handle before erasing.
    if (curlEasyHandle != nullptr) {
        curl_easy_cleanup(curlEasyHandle);
    }
    curl_slist_free_all(curlRequestHeaders);
    JS_FreeValueRT(jsRuntime, readyResolve);
    JS_FreeValueRT(jsRuntime, readyReject);
    JS_FreeValueRT(jsRuntime, readResolve);
    JS_FreeValueRT(jsRuntime, readReject);
}

FetchClient::FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> callbackDidRun) {
    if (jsContext == nullptr || uvLoop == nullptr || !callbackDidRun) {
        throw std::invalid_argument("FetchClient requires a context, initialized loop, and completion hook");
    }
    this->jsRuntime = JS_GetRuntime(jsContext);
    this->jsContext = jsContext;
    this->uvLoop = uvLoop;
    this->callbackDidRun = std::move(callbackDidRun);
    this->ownerThreadID = this_thread::get_id();
    try {
        static std::once_flag curlInitialization;
        std::call_once(curlInitialization, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                throw std::runtime_error("curl_global_init failed");
            }
        });
        curlMultiHandle = curl_multi_init();
        if (curlMultiHandle == nullptr) {
            throw std::bad_alloc();
        }
        {
            std::unique_ptr<uv_timer_t> uvTimerHandle = std::make_unique<uv_timer_t>();
            const int status = uv_timer_init(uvLoop, uvTimerHandle.get());
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvTimerHandle->data = this;
            uvTimeoutHandle = uvTimerHandle.release();
        }
        {
            std::unique_ptr<uv_async_t> uvAsync = std::make_unique<uv_async_t>();
            const int status = uv_async_init(uvLoop, uvAsync.get(), [](uv_async_t* uvAsync) noexcept {
                try {
                    FetchClient* fetchClient = static_cast<FetchClient*>(uvAsync->data);
                    if (fetchClient == nullptr) {
                        return;
                    }
                    // This notification is consumed. Scheduling more delivery refs
                    // the handle again; otherwise the idle client keeps no loop alive.
                    uv_unref(reinterpret_cast<uv_handle_t*>(uvAsync));
                    deliver(*fetchClient);
                    // Delivery may shut down the client and close this handle.
                } catch (...) {
                    std::terminate();
                }
            });
            if (status != 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            uvAsync->data = this;
            uvAsyncHandle = uvAsync.release();
            uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle));
        }
        const std::vector<CURLMcode> optionResults = {
            curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETFUNCTION, socketInterest),
            curl_multi_setopt(curlMultiHandle, CURLMOPT_SOCKETDATA, this),
            curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERFUNCTION, timerInterest),
            curl_multi_setopt(curlMultiHandle, CURLMOPT_TIMERDATA, this),
        };
        for (const CURLMcode result : optionResults) {
            if (result != CURLM_OK) {
                throw std::runtime_error(curl_multi_strerror(result));
            }
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
    while (!fetchStates.empty()) {
        closeResponse(*this, fetchStates.begin()->first, false);
    }
    if (curlMultiHandle != nullptr) {
        if (curl_multi_cleanup(curlMultiHandle) != CURLM_OK) {
            std::terminate();
        }
        curlMultiHandle = nullptr;
    }
    for (const std::pair<const curl_socket_t, uv_poll_t*>& entry : socketMap) {
        closePollHandle(entry.second);
    }
    socketMap.clear();
    closeTimerHandle(uvTimeoutHandle);
    uvTimeoutHandle = nullptr;
    if (uvAsyncHandle != nullptr) {
        uvAsyncHandle->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(uvAsyncHandle), [](uv_handle_t* uvHandle) {
            delete reinterpret_cast<uv_async_t*>(uvHandle);
        });
        uvAsyncHandle = nullptr;
    }
}

JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient) {
    try {
        if (this_thread::get_id() != fetchClient.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != fetchClient.jsContext || fetchClient.shuttingDown || fetchClient.responseClassID != 0) {
            JS_ThrowInternalError(jsContext, "FetchClient registration requires its live context and a fresh client");
            return nullptr;
        }
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        JSClassID clientClassID = 0;
        JS_NewClassID(jsRuntime, &clientClassID);
        const JSClassDef clientClass = {
            .class_name = "FetchClient",
            .finalizer = nullptr,
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, clientClassID, &clientClass) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        JS_NewClassID(jsRuntime, &fetchClient.responseClassID);
        const JSClassDef responseClass = {
            .class_name = "FetchResponse",
            .finalizer = [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                try {
                    const std::unique_ptr<FetchResponse> fetchResponse(static_cast<FetchResponse*>(JS_GetOpaque(value, JS_GetClassID(value))));
                    if (fetchResponse != nullptr && fetchResponse->curlEasyHandle != nullptr && !fetchResponse->fetchClient->shuttingDown) {
                        FetchClient* fetchClient = fetchResponse->fetchClient;
                        if (fetchClient->fetchStates.contains(fetchResponse->curlEasyHandle)) {
                            // GC can run during delivery allocations. Do not erase a
                            // FetchState that delivery is currently accessing, and do
                            // not settle Promises from inside a QuickJS finalizer.
                            fetchClient->fetchStates.at(fetchResponse->curlEasyHandle).closeRequested = true;
                            scheduleDelivery(*fetchClient);
                        }
                    }
                } catch (...) {
                    std::terminate();
                }
            },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, fetchClient.responseClassID, &responseClass) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        ScopedJSValue client(jsContext, JS_NewObjectClass(jsContext, clientClassID));
        if (JS_IsException(client.get())) {
            return nullptr;
        }
        if (JS_SetOpaque(client.get(), &fetchClient) < 0) {
            JS_ThrowInternalError(jsContext, "Could not attach FetchClient");
            return nullptr;
        }
        JSValueConst clientData = client.get();
        const JSValue send = JS_NewCFunctionData2(jsContext, FetchClient::send, "send", 1, 0, 1, &clientData);
        if (JS_IsException(send) || JS_DefinePropertyValueStr(jsContext, client.get(), "send", send, JS_PROP_ENUMERABLE) < 0) {
            return nullptr;
        }
        ScopedJSValue prototype(jsContext, JS_NewObject(jsContext));
        if (JS_IsException(prototype.get())) {
            return nullptr;
        }
        const std::array<std::pair<const char*, JSCFunctionData*>, 2> methods = {{
            {"read", FetchResponse::read},
            {"close", FetchResponse::close},
        }};
        for (const std::pair<const char*, JSCFunctionData*>& method : methods) {
            const JSValue function = JS_NewCFunctionData2(jsContext, method.second, method.first, 0, 0, 1, &clientData);
            if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, prototype.get(), method.first, function, 0) < 0) {
                return nullptr;
            }
        }
        const std::array<std::pair<const char*, JSCFunctionData*>, 3> getters = {{
            {"status", FetchResponse::getStatus},
            {"headers", FetchResponse::getHeaders},
            {"url", FetchResponse::getURL},
        }};
        for (const std::pair<const char*, JSCFunctionData*>& getter : getters) {
            const JSValue function = JS_NewCFunctionData2(jsContext, getter.second, getter.first, 0, 0, 1, &clientData);
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
        JS_SetClassProto(jsContext, fetchClient.responseClassID, prototype.release());
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
