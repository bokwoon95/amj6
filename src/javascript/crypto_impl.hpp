#pragma once

#include "javascript.hpp"

#include <quickjs.h>
#include <uv.h>

#include <functional>
#include <map>
#include <thread>

namespace amj::javascript {

// Host-owned digest service, constructed on the context/loop's owner thread.
// Neither the context nor the initialized libuv loop is owned or run here.
// Keep this object at a stable address and do not modify its fields directly.
//
// jsExecutePendingJobs runs after each digest-completion callback, including JS failures.
// It must consume/report pending exceptions and drain QuickJS jobs. It may call
// shutdown(), but must not destroy this object/context, nest the loop, or throw.
// Captures must be native-only and remain valid until this object is destroyed.
//
// Explicitly shutdown before freeing the context. Shutdown detaches pending
// work and releases JS callbacks without invoking them; abandoned digest
// Promises are not settled. Work is not canceled. Keep driving the borrowed
// loop until queued work/completions finish before closing/destroying the loop.
// Detached completions only free native storage and no longer borrow this owner
// or its context. Keep this object alive through context/runtime destruction.
// After shutdown(), repeated shutdown()/destruction do not access QuickJS.
struct Crypto {
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    uv_loop_t* uvLoop = nullptr;
    std::function<void(JSContext*)> jsExecutePendingJobs = {};
    std::thread::id ownerThreadID = {};
    std::map<uv_work_t*, OwnedJSValue> pendingDigests = {}; // Host-owned JS references; released on completion/shutdown.
    bool shuttingDown = false;

    Crypto(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> jsExecutePendingJobs);
    ~Crypto();
    Crypto(const Crypto&) = delete;
    Crypto& operator=(const Crypto&) = delete;
    Crypto(Crypto&&) = delete;
    Crypto& operator=(Crypto&&) = delete;

    // Requires the owner thread; first call requires a live context/runtime.
    // Later calls are safe after QuickJS destruction. Does not run JS or alter
    // pending exceptions. Permanently rejects new digest requests.
    void shutdown();
};

// Fills an integer typed array synchronously using uv_random(), without a loop.
// Returns the same array, or JS_EXCEPTION. Maximum size: 65,536 bytes.
JSValue cryptoGetRandomValues(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// Normalized arguments: SHA-1/256/384/512 name, Uint8Array, completion callback.
// Copies input before returning; worker threads never access JS values/storage.
// Completion receives (error, result), with null error on success and null result
// on failure. Returns undefined, or JS_EXCEPTION if submission fails.
JSValue cryptoDigest(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, Crypto& crypto);

// Registers amj:crypto_impl. Call once per context before loading crypto.js.
// Borrows crypto, including on failure; never deletes it. The host shuts it down
// before freeing the context and keeps it alive through runtime destruction.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createCryptoImplModule(JSContext* jsContext, Crypto& crypto);

} // namespace amj::javascript
