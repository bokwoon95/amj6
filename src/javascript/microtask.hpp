#pragma once

#include <quickjs.h>

namespace amj::javascript {

// Enqueues a callable first argument, returning undefined or JS_EXCEPTION.
// QuickJS retains its own callback reference until execution or runtime cleanup.
// The host must keep the context alive, drain jobs with JS_ExecutePendingJob(),
// and report callback exceptions without abandoning the remaining microtasks.
JSValue queueMicrotask(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// Registers amj:microtask, exporting queueMicrotask. Call once per context before
// loading consumers. The host chooses whether to expose the export globally.
// Returns an engine-managed module, or nullptr with a pending exception.
JSModuleDef* createMicrotaskModule(JSContext* jsContext);

} // namespace amj::javascript
