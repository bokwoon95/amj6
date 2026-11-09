#pragma once

#include "javascript.hpp"

#include <curl/curl.h>
#include <curl/multi.h>
#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {

struct FetchRequest {
    std::string url = "";
    std::string method = "";
    std::vector<std::pair<std::string, std::string>> headers = {};
    std::vector<uint8_t> body = {};
};

// Client-owned operation storage. Curl callbacks borrow this object, not its
// parent client. Its address remains stable until the easy handle is removed.
struct FetchState {
    CURLM* curlMultiHandle = nullptr;
    CURL* ownedCurlEasyHandle = nullptr;
    curl_slist* ownedCurlRequestHeaders = nullptr;
    FetchRequest fetchRequest = {};
    int64_t statusCode = 0;
    std::string responseURL = "";
    std::vector<std::pair<std::string, std::string>> responseHeaders = {};

    std::deque<std::vector<uint8_t>> chunks = {};
    std::size_t bufferedBytes = 0;
    std::size_t headerBytes = 0;
    std::string errorMessage = "";

    JSPromise headersReceived = {};
    std::optional<JSPromise> read = {};

    bool responseHeadersReceived = false; // Response headers received.
    bool finished = false;                // Network transfer ended.
    bool paused = false;                  // Body reception paused for backpressure.
    bool redirecting = false;             // Currently processing a redirect response.
    bool closeRequested = false;          // Deferred cleanup requested.

    FetchState() = default;
    ~FetchState();
    FetchState(const FetchState&) = delete;
    FetchState& operator=(const FetchState&) = delete;
    FetchState(FetchState&&) = delete;
    FetchState& operator=(FetchState&&) = delete;
};

// Host-owned, one per context/loop pair, at a stable address. Borrows the context
// and initialized loop. All use/destruction requires their owner thread.
//
// callbackDidRun reports/consumes JS exceptions and drains QuickJS jobs after
// delivered completions. It may call shutdown(), but must not destroy this
// object/context, nest the loop, or throw. Captures must be native-only.
//
// Explicitly shutdown while QuickJS is alive, drain native close callbacks, then
// free the context/runtime before destroying this object. Repeated shutdown and
// destruction after shutdown never access QuickJS. Detached libuv callbacks only
// release native storage. No module carrier owns or GC-marks this object.
struct FetchClient {
    uv_loop_t* uvLoop = nullptr;
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    CURLM* ownedCurlMultiHandle = nullptr;
    uv_timer_t* ownedUvTimerHandle = nullptr;
    uv_async_t* ownedUvAsyncHandle = nullptr;
    std::map<curl_socket_t, uv_poll_t*> ownedUvPollHandleMap = {};
    std::map<CURL*, FetchState> fetchStateMap = {};
    std::function<void(JSContext*)> callbackDidRun = {};
    std::thread::id ownerThreadID = {};
    JSClassID responseClassID = 0;
    bool shuttingDown = false;

    FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> callbackDidRun);
    ~FetchClient();
    FetchClient(const FetchClient&) = delete;
    FetchClient& operator=(const FetchClient&) = delete;
    FetchClient(FetchClient&&) = delete;
    FetchClient& operator=(FetchClient&&) = delete;

    // Idempotent. Releases JS references without settling abandoned Promises.
    // First call requires live QuickJS; later calls only check the owner thread.
    void shutdown();

    // Direct QuickJS callback. functionData[0] retains the borrowed client carrier.
    static JSValue send(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);
};

// Owned by the opaque JS object. The client outlives all such objects.
// A null easy handle means closed; shutdown clients reject all further access.
struct FetchResponse {
    FetchClient* fetchClient = nullptr;
    CURL* curlEasyHandle = nullptr;

    // Direct QuickJS callbacks. Extract this response from thisValue;
    // functionData[0] supplies the bound client's class for receiver validation.
    static JSValue read(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);
    static JSValue close(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);

    // Getter-only JS properties: status, headers, and url. headersReceived is a read-only
    // Promise property installed on each JS response by FetchClient::send().
    static JSValue getStatus(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);
    static JSValue getHeaders(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);
    static JSValue getURL(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);
};

// Registers amj:fetch_impl, exporting fetchClient with send(fetchRequest).
// Borrows fetchClient even on failure. Call once per context before fetch.js.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient);

} // namespace amj::javascript
