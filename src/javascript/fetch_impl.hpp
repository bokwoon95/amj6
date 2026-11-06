#pragma once

#include <quickjs.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace amj::javascript {

class Scheduler;
struct FetchImplState;

// Plain completion data, converted to JS objects with the same field names.
struct FetchFailure {
    std::string type = "";
    std::string message = "";
};

struct FetchResponseMetadata {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers = {};
};

// Native HTTP operations used by fetch.js. JS owns Promises, abort reasons and
// listeners, Error/Response objects, and public argument normalization. Native
// code owns workers, bounded buffers, and one-shot scheduler completion callbacks.
// All methods require the scheduler/context owner thread and matching JSContext.
class FetchImpl {
  public:
    // url/method: normalized strings; headerPairs: array of two-string arrays;
    // bytes: Uint8Array; onHeaders: function(failureOrNull, metadataOrNull).
    // Returns an opaque JS request handle (FetchRequestHandle in fetch_impl.d.ts),
    // or JS_EXCEPTION on synchronous failure.
    // Later invokes onHeaders once through the scheduler with FetchFailure/null and
    // FetchResponseMetadata/null. Header pairs become two-element JS arrays.
    // No native Promise or JS Error is created. 4xx/5xx are successful responses.
    JSValue start(JSContext* jsContext, JSValueConst url, JSValueConst method, JSValueConst headerPairs, JSValueConst bytes, JSValueConst onHeaders);

    // request: handle from this FetchImpl instance, after successful headers.
    // onRead: function(failureOrNull, chunkOrNull). One outstanding read is allowed.
    // Later invokes onRead once through the scheduler. chunk is Uint8Array; null
    // with no failure means EOF. Returns undefined, or JS_EXCEPTION on failure.
    JSValue read(JSContext* jsContext, JSValueConst request, JSValueConst onRead);

    // Stops/joins the worker and unregisters pending callbacks. Idempotent for a
    // closed handle. Drops completion functions without invoking them. Returns
    // undefined, or JS_EXCEPTION on failure. JS settles its own cancelled Promises.
    JSValue close(JSContext* jsContext, JSValueConst request);

    // Registers amj:fetch_impl, exporting fetchImpl bound to this scheduler.
    // Call once per context before loading fetch.js. Returns a context-owned
    // module, or nullptr with a pending exception.
    // The scheduler/context are borrowed. Clear/destroy the scheduler before the
    // context. Cleanup invalidates the instance and joins workers without calling
    // JS; use fresh registration/context after clear(). Callbacks hold weak references,
    // so destroying scheduler callbacks cannot join workers under its mutex.
    //
    // HTTP(S) only, verified TLS, no credentials/redirects/retries/cookies. Limits:
    // 64 requests, 64 KiB headers/URL, 32 MiB upload, 64 KiB chunks, 256 KiB queued
    // body per request. Timeouts: 10s connect, 60s read, 30s write. DNS/connect/OS
    // calls may delay cancellation/join: use off the UI thread.
    static JSModuleDef* createModule(JSContext* jsContext, Scheduler& scheduler);

  private:
    std::shared_ptr<FetchImplState> fetchImplState = nullptr;
};

} // namespace amj::javascript
