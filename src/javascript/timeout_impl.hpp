#pragma once

#include <quickjs.h>

#include <cstdint>
#include <map>

namespace amj::javascript {

class Scheduler;

// Owner-managed, one per scheduler/context pair. Bindings and callbacks borrow
// this object; keep it at a stable address and do not copy or modify it while in
// use. The borrowed scheduler must not change. All use and destruction require
// the scheduler/context owner thread. The owner must stop JS calls and clear all
// callbacks before destroying this object; the context must outlive the scheduler
// and retained JS values. scheduler.clear() leaves this object and module reusable.
struct Timeout {
    Scheduler* scheduler = nullptr; // Borrowed from the enclosing owner.
    int32_t timerIDCounter = 0; // Last allocated timer ID; allocation starts at 1.
    std::map<int32_t, int64_t> registry = {}; // JS timer ID -> scheduler callback ID.
};

// callback: function; delay: normalized integer milliseconds in [0, 2147483647].
// Returns a timer ID in [1, INT32_MAX], or JS_EXCEPTION. JS owns coercion,
// clamping, and forwarding extra callback arguments.
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
