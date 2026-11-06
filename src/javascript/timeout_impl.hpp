#pragma once

#include <quickjs.h>
#include <uv.h>

#include <cstdint>
#include <functional>
#include <map>
#include <thread>

namespace amj::javascript {

struct TimeoutTimer;

// Owner-managed, one per context/loop pair. Neither the context nor the loop is
// owned or run here. All operations and destruction require the constructing
// thread, which must also run the loop and own the context. Keep this object at
// a stable address; do not modify its fields after construction.
//
// afterCallback runs after every timer invocation, including JS failure. It must
// consume any pending exception, apply the host's error policy, and drain QuickJS
// jobs (handling their errors too). It may call shutdown(), but must not destroy
// this object or the context, run a nested event loop, or throw. A throwing hook
// terminates the process rather than unwinding through libuv.
//
// shutdown() releases pending timers' JS values and starts native handle closure;
// an executing callback keeps its local JS references until it returns.
// shutdown() does not run the loop or wait for close callbacks. Those callbacks only
// delete native storage and no longer borrow this object or the context. Stop
// calls through the JS module before destroying this object. The context must
// remain alive through shutdown()/destruction. The host must finish native close
// callbacks before closing/destroying the loop.
struct Timeout {
    JSContext* jsContext = nullptr;
    uv_loop_t* uvLoop = nullptr;
    std::function<void(JSContext*)> afterCallback = {};
    std::thread::id ownerThreadID = {};
    int32_t timerIDCounter = 0; // Last allocated timer ID; allocation starts at 1.
    std::map<int32_t, TimeoutTimer*> registry = {}; // Public timer ID -> native timer.
    bool shuttingDown = false;

    // Throws invalid_argument for missing dependencies. Does not initialize or
    // run the borrowed loop; the caller must supply an initialized loop.
    Timeout(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> afterCallback);
    ~Timeout();
    Timeout(const Timeout&) = delete;
    Timeout& operator=(const Timeout&) = delete;
    Timeout(Timeout&&) = delete;
    Timeout& operator=(Timeout&&) = delete;

    // Idempotent; permanently rejects new timers. Does not execute JS callbacks
    // or alter a pending JS exception. Wrong-thread use terminates the process.
    void shutdown();
};

// callback: function; delay: normalized integer milliseconds in [0, 2147483647].
// Returns a timer ID in [1, INT32_MAX], or JS_EXCEPTION. JS owns coercion,
// clamping, and forwarding extra callback arguments. Requires timeout's context
// and owner thread. Libuv's zero-delay timer runs on a subsequent loop iteration.
JSValue timeoutSet(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, Timeout& timeout);

// timerID: nonnegative safe integer. Unknown/cleared IDs have no effect.
// Returns undefined, or JS_EXCEPTION. Only cancels this state's timers.
JSValue timeoutClear(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, Timeout& timeout);

// Registers amj:timeout_impl, exporting timeoutSet and timeoutClear. Call once
// per context before loading timeout.js. Borrows timeout; the owner must stop
// calls through this module before destroying it. Returns a context-owned module,
// or nullptr with a pending exception.
JSModuleDef* createTimeoutImplModule(JSContext* jsContext, Timeout& timeout);

} // namespace amj::javascript
