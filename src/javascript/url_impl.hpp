#pragma once

#include <quickjs.h>

namespace amj::javascript {

// Stateless Ada callbacks used by url.js. All require the context's owner thread.
// Inputs are already-coerced strings; each callback validates its own arguments.
// Failures return JS_EXCEPTION with a pending exception. Limits: 1 MiB UTF-8
// input/output/query and 65536 query pairs. Nothing is installed globally.

// input: string; base: optional string (missing/undefined means no base).
// Returns a plain object matching URLParts in url_impl.d.ts, or null for invalid URLs.
JSValue urlParse(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// href/component/value: strings. Returns a new URLParts object, or null when Ada
// rejects the URL/change. Unknown component names throw. Does not mutate input.
JSValue urlWithComponent(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// queryString: string, optionally prefixed by '?'. Returns ordered [name, value]
// pairs, preserving duplicates, using application/x-www-form-urlencoded rules.
JSValue queryStringParse(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// pairs: array of two-string arrays. Returns an encoded query without '?'.
JSValue queryStringify(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv);

// Registers amj:url_impl with the four named function exports above. Call once
// per context before loading url.js. Returns a context-owned module, or nullptr
// with a pending exception. No scheduler is needed.
JSModuleDef* createURLImplModule(JSContext* jsContext);

} // namespace amj::javascript
