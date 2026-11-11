#pragma once

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "void_ptr/void_ptr.hpp"

#include <curl/curl.h>
#include <curl/multi.h>
#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace errors = amj::errors;
namespace void_ptr = amj::void_ptr;

struct FetchRequest {
    std::string url = "";
    std::string method = "";
    std::vector<std::pair<std::string, std::string>> headers = {};
    std::vector<uint8_t> body = {};
};

enum class FetchStatus : std::uint8_t {
    Running,
    Paused,
    Done,
};

// Client-owned operation storage. Curl callbacks borrow this object, not its
// parent client. Its address remains stable until the easy handle is removed.
struct FetchState {
    std::unique_ptr<void_ptr::VoidPtr> selfVoidPtr = nullptr;
    uint64_t fetchStateID = 0;
    CURLM* curlMultiHandle = nullptr;
    CURL* ownedCurlEasyHandle = nullptr;
    curl_slist* ownedCurlRequestHeaders = nullptr;
    FetchRequest fetchRequest = {};
    int64_t statusCode = 0;
    std::string effectiveURL = "";
    std::vector<std::pair<std::string, std::string>> responseHeaders = {};
    std::size_t responseHeadersSize = 0;
    bool responseHeadersComplete = false;
    std::deque<std::vector<uint8_t>> responseBodyChunks = {};
    std::size_t responseBodySize = 0;
    errors::Error error = {};

    FetchStatus fetchStatus = FetchStatus::Running;
    bool redirecting = false; // Currently processing a redirect response.
    bool abandoned = false;   // JavaScript code stopped referencing it before the fetchResponse was closed.

    OwnedJSPromise readyPromise = {};
    std::optional<OwnedJSPromise> readPromise = {};

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
// jsExecutePendingJobs reports/consumes pending JS exceptions and executes queued
// QuickJS jobs until none remain, handling job exceptions too. It runs after
// native promise settlement. It must not run/pump libuv or another event loop,
// shut down or destroy any native module service, free the context/runtime, or
// throw. Host teardown must wait until JS execution and native callbacks return;
// JS must not resume after teardown begins. Captures must be native-only.
//
// Explicitly shutdown while QuickJS is alive, drain native close callbacks, then
// free the context/runtime before destroying this object. Repeated shutdown and
// destruction after shutdown never access QuickJS. Detached libuv callbacks only
// release native storage. No module carrier owns or GC-marks this object.
struct FetchClient {
    std::unique_ptr<void_ptr::VoidPtr> selfVoidPtr = nullptr; // Owns the wrapper, which borrows this client.
    uv_loop_t* uvLoop = nullptr;
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    CURLM* ownedCurlMultiHandle = nullptr;
    uv_timer_t* ownedUvTimerHandle = nullptr;
    uv_async_t* ownedUvAsyncHandle = nullptr;
    std::map<curl_socket_t, uv_poll_t*> ownedUvPollHandleMap = {};
    std::map<CURL*, FetchState> fetchStateMap = {};
    uint64_t fetchStateIDCounter = 0;
    std::function<void(JSContext*)> jsExecutePendingJobs = {};
    std::thread::id ownerThreadID = {};
    bool shuttingDown = false;

    FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> jsExecutePendingJobs);
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

    // Direct QuickJS callbacks. Extract this response from thisValue.
    static JSValue read(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);
    static JSValue close(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

    // Getter-only JS properties: status, headers, and url. ready is a read-only
    // Promise property installed on each JS response by FetchClient::send().
    static JSValue getStatus(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);
    static JSValue getHeaders(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);
    static JSValue getURL(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);
};

// Registers amj:fetch_impl, exporting fetchClient with send(fetchRequest).
// Borrows fetchClient even on failure. Call once per context before fetch.js.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createFetchImplModule(JSContext* jsContext, FetchClient& fetchClient);

} // namespace amj::javascript
