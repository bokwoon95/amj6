#pragma once

#include <curl/curl.h>
#include <curl/multi.h>
#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
    FetchRequest fetchRequest = {};
    curl_slist* curlRequestHeaders = nullptr;
    CURL* curlEasyHandle = nullptr;
    int64_t statusCode = 0;
    std::string responseURL = "";
    std::vector<std::pair<std::string, std::string>> responseHeaders = {};
    std::deque<std::vector<uint8_t>> chunks = {};
    std::size_t bufferedBytes = 0;
    std::size_t headerBytes = 0;
    std::string failure = "";
    JSRuntime* jsRuntime = nullptr;
    JSValue readyResolve = JS_UNDEFINED;
    JSValue readyReject = JS_UNDEFINED;
    JSValue readResolve = JS_UNDEFINED;
    JSValue readReject = JS_UNDEFINED;
    bool addedToMulti = false;
    bool headersReceived = false;
    bool readyDelivered = false;
    bool finished = false;
    bool paused = false;
    bool redirecting = false;
    bool closeRequested = false; // JS finalizers defer curl cleanup to delivery.

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
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    uv_loop_t* uvLoop = nullptr;
    CURLM* curlMultiHandle = nullptr;
    uv_timer_t* curlTimer = nullptr;
    uv_timer_t* deliveryTimer = nullptr;
    std::map<curl_socket_t, uv_poll_t*> socketMap = {};
    std::map<CURL*, FetchState> fetchStates = {};
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

    // Internal driver operations; do not call from application code.
    void drive(curl_socket_t socket, int events);
    void scheduleDelivery();
    void deliver();
    void close(CURL* curlEasyHandle, bool settle);
};

// Owned by the opaque JS object. The client outlives all such objects.
// A null easy handle means closed; shutdown clients reject all further access.
struct FetchResponse {
    FetchClient* fetchClient = nullptr;
    CURL* curlEasyHandle = nullptr;
};

// Registers amj:fetch_impl, exporting fetchClient with send(fetchRequest).
// Borrows fetchClient even on failure. Call once per context before fetch.js.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient);

} // namespace amj::javascript
