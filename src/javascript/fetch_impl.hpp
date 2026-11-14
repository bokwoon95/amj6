#pragma once

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "owned/uv.hpp"
#include "void_ptr/void_ptr.hpp"

#include <curl/curl.h>
#include <curl/multi.h>
#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace amj::javascript {
namespace errors = amj::errors;
namespace owned = amj::owned;
namespace void_ptr = amj::void_ptr;

enum class CurlTransferStatus : std::uint8_t {
    NotStarted,
    Running,
    Paused,
    Completed,
};

struct FetchRequest {
    std::string url = "";
    std::string method = "";
    std::vector<std::pair<std::string, std::string>> headers = {};
    std::vector<uint8_t> body = {};
};

// Client-owned operation storage. Curl callbacks borrow this object, not its
// parent client. Its address remains stable until the easy handle is removed.
struct FetchState {
    std::unique_ptr<void_ptr::VoidPtr> selfVoidPtr = nullptr;
    uint64_t fetchStateID = 0;
    CURLM* curlMultiHandle = nullptr;
    CURL* ownedCurlEasyHandle = nullptr;
    errors::Error error = {};
    bool abandoned = false; // JavaScript code stopped referencing it before the fetchResponse was closed.

    // Request.
    curl_slist* ownedCurlRequestHeaders = nullptr;
    FetchRequest request = {};

    // Response.
    CurlTransferStatus responseStatus = CurlTransferStatus::NotStarted;
    int64_t responseCode = 0;
    std::string effectiveURL = "";
    std::vector<std::pair<std::string, std::string>> responseHeaders = {};
    std::size_t responseHeadersSize = 0;
    std::size_t responseTrailersSize = 0;
    bool responseHeadersComplete = false;
    std::deque<std::vector<uint8_t>> responseBuffer = {};
    std::size_t responseBufferSize = 0;
    bool responseRedirecting = false; // Currently processing a redirect response.
    OwnedJSPromise responseReadyPromise = {};
    std::optional<OwnedJSPromise> responseReadPromise = {};

    FetchState() = default;
    ~FetchState();
    FetchState(const FetchState&) = delete;
    FetchState& operator=(const FetchState&) = delete;
    FetchState(FetchState&&) = delete;
    FetchState& operator=(FetchState&&) = delete;
};

// Host-owned, one per context/loop pair, at a stable address. Borrows the context
// and initialized loop. All use/destruction requires their owner thread.
// Borrows the host's pendingError variant shared by modules on the same loop.
// Preserve the first error; use hasError() to check its value. After uv_run or
// direct JS execution returns, the host must inspect it before resuming work.
// Pending errors suspend fetch work; shut down this client before clearing the
// variant with std::error_code(). Promise rejection/recreation is a host decision.
//
// jsExecutePendingJobs reports/consumes pending JS exceptions and executes queued
// QuickJS jobs until none remain, handling job exceptions too. If a native JS
// call reports pendingError, stop draining jobs and return to the adapter/host.
// It runs after native promise settlement. It must not run/pump libuv or another event loop,
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
    std::variant<std::error_code, std::exception_ptr>* pendingError = nullptr; // Borrowed; outlives shutdown.
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    CURLM* ownedCurlMultiHandle = nullptr;
    owned::UvHandle<uv_timer_t> uvTimerHandle = {};
    owned::UvHandle<uv_async_t> uvAsyncHandle = {};
    std::map<curl_socket_t, owned::UvHandle<uv_poll_t>> uvPollHandleMap = {};
    std::map<CURL*, FetchState> fetchStateMap = {};
    uint64_t fetchStateIDCounter = 0;
    std::function<void(JSContext*)> jsExecutePendingJobs = {};
    std::thread::id ownerThreadID = {};
    bool shuttingDown = false; // Used only by shutdown for idempotency/reentrancy.

    FetchClient(JSContext* jsContext, uv_loop_t* uvLoop, std::variant<std::error_code, std::exception_ptr>& pendingError, std::function<void(JSContext*)> jsExecutePendingJobs);
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
// A null easy handle means closed. The host must not run JS after shutdown.
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
