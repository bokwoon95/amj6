#pragma once

#include "errors/errors.hpp"
#include "owned/quickjs.hpp"
#include "owned/uv.hpp"
#include "void_ptr/void_ptr.hpp"

#include <curl/curl.h>
#include <curl/multi.h>
#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
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
namespace owned = amj::owned;
namespace this_thread = std::this_thread;
namespace void_ptr = amj::void_ptr;

enum class CurlTransferStatus : std::uint8_t {
    NotStarted,
    Running,
    Paused,
    Completed,
};

class FetchRequest {
public:
    static std::optional<FetchRequest> fromJSValue(JSContext& jsContext, JSValueConst jsValue);

    std::string method = "";

    std::string url = "";

    std::vector<std::pair<std::string, std::string>> headers = {};

    std::vector<uint8_t> body = {};
};

// Client-owned operation storage. Curl callbacks borrow this object, not its
// parent client. Its address remains stable until the easy handle is removed.
class FetchState {
public:
    std::unique_ptr<void_ptr::VoidPtr> curlVoidPtr = nullptr;

    uint64_t fetchStateID = 0;

    CURLM* curlMultiHandle = nullptr;

    CURL* ownedCurlEasyHandle = nullptr;

    errors::Error error = {};

    bool abandoned = false;

    // Request.

    curl_slist* ownedCurlRequestHeaders = nullptr;

    FetchRequest request = {};

    // Response.

    CurlTransferStatus responseCurlTransferStatus = CurlTransferStatus::NotStarted;

    int64_t responseCode = 0;

    std::string effectiveURL = "";

    std::vector<std::pair<std::string, std::string>> responseHeaders = {};

    std::size_t responseHeadersSize = 0;

    std::size_t responseTrailersSize = 0;

    bool responseHeadersComplete = false;

    std::deque<std::vector<uint8_t>> responseBuffer = {};

    std::size_t responseBufferSize = 0;

    bool responseRedirecting = false;

    owned::JSPromise responseReadyPromise = {};

    std::optional<owned::JSPromise> responseReadPromise = {};

    FetchState() = default;

    ~FetchState();

    // Idempotent native teardown; releases JS references without settling Promises.
    // Normal return means detached and cleaned up, even when an error is returned.
    // First call requires live QuickJS. Do not use this state after closing it.
    [[nodiscard]] errors::Error close() noexcept;

    FetchState(const FetchState&) = delete;

    FetchState& operator=(const FetchState&) = delete;

    FetchState(FetchState&&) = delete;

    FetchState& operator=(FetchState&&) = delete;

    static std::size_t curlHeaderFunction(char* buffer, std::size_t size, std::size_t nitems,
        void* userdata) noexcept;

    static std::size_t curlWriteFunction(char* ptr, std::size_t size, std::size_t nmemb,
        void* userdata) noexcept;
};

// Host-owned, one per context/loop pair, at a stable address. Borrows the context
// and initialized loop. All use/destruction requires their owner thread.
// Borrows the host's pendingError shared by modules on the same loop.
// Preserve the first error; use hasValue() to check its value. After uv_run or
// direct JS execution returns, the host must inspect it before resuming work.
// Pending errors suspend fetch work; close this client before clearing the
// error with errors::Error(). Promise rejection/recreation is a host decision.
//
// jsExecutePendingJobs reports/consumes pending JS exceptions and executes queued
// QuickJS jobs until none remain, handling job exceptions too. If a native JS
// call reports pendingError, stop draining jobs and return to the adapter/host.
// It runs after native promise settlement. It must not run/pump libuv or another event loop,
// close or destroy any native module service, free the context/runtime, or
// throw. Host teardown must wait until JS execution and native callbacks return;
// JS must not resume after teardown begins. Captures must be native-only.
//
// Explicitly close while QuickJS is alive, drain native close callbacks, then
// free the context/runtime before destroying this object. Repeated close() and
// destruction after close() never access QuickJS. Detached libuv callbacks only
// release native storage. No module carrier owns or GC-marks this object.
class FetchClient {
public:
    std::thread::id ownerThreadID = this_thread::get_id();

    errors::Error& pendingError;

    std::map<CURL*, FetchState> fetchStateMap = {};

    owned::UvHandle<uv_async_t> uvAsyncHandle = {};

    static std::expected<std::unique_ptr<FetchClient>, errors::Error> create(JSContext& jsContext,
        uv_loop_t& uvLoop, errors::Error& pendingError,
        std::function<void(JSContext&)> jsExecutePendingJobs);

    ~FetchClient();
    FetchClient(const FetchClient&) = delete;
    FetchClient& operator=(const FetchClient&) = delete;
    FetchClient(FetchClient&&) = delete;
    FetchClient& operator=(FetchClient&&) = delete;

    static JSValue send(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

    [[nodiscard]] errors::Error close() noexcept;

private:
    uint64_t fetchStateIDCounter = 0;

    bool closed = false;

    JSContext& jsContext;

    uv_loop_t& uvLoop;

    std::function<void(JSContext&)> jsExecutePendingJobs = {};

    std::unique_ptr<void_ptr::VoidPtr> curlVoidPtr = nullptr;

    CURLM* ownedCurlMultiHandle = nullptr;

    owned::UvHandle<uv_timer_t> uvTimerHandle = {};

    std::map<curl_socket_t, owned::UvHandle<uv_poll_t>> uvPollHandleMap = {};

    FetchClient(JSContext& jsContext, uv_loop_t& uvLoop, errors::Error& pendingError);

    [[nodiscard]] bool driveCurlTransfers(curl_socket_t socket, int eventBitmask);

    static void uvAsyncCallback(uv_async_t* uvAsyncHandle) noexcept;

    static int curlMSocketFunction(CURL* curlEasyHandle, curl_socket_t socket, int what,
        void* clientp, void* socketp) noexcept;

    static int curlMTimerFunction(CURLM* curlMultiHandle, long timeout_ms, void* clientp) noexcept;
};

class FetchResponse {
public:
    explicit FetchResponse(FetchClient& fetchClient);

    FetchClient& fetchClient;

    CURL* curlEasyHandle = nullptr;

    static std::optional<std::reference_wrapper<FetchResponse>> fromThisValue(JSContext& jsContext,
        JSValueConst thisValue);

    static JSValue read(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

    static JSValue close(JSContext* jsContext, JSValueConst thisValue, int argc,
        JSValueConst* argv);

    static JSValue getStatus(JSContext* jsContext, JSValueConst thisValue, int argc,
        JSValueConst* argv);

    static JSValue getHeaders(JSContext* jsContext, JSValueConst thisValue, int argc,
        JSValueConst* argv);

    static JSValue getURL(JSContext* jsContext, JSValueConst thisValue, int argc,
        JSValueConst* argv);
};

// Registers amj:fetch_impl, exporting fetchClient with send(fetchRequest).
// Borrows fetchClient even on failure. Call once per context before fetch.js.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createFetchImplModule(JSContext& jsContext, FetchClient& fetchClient);

} // namespace amj::javascript
