# JavaScript modules

Dependency order: **text -> URL -> scheduler -> timeout -> fetch**. JavaScript
facades export their public API; they do not install anything on the global object.
The host chooses which target receives those exports.

## Modules and native/JS boundary

| Module name        | Implementation          | Exports                                                              |
| ------------------ | ----------------------- | -------------------------------------------------------------------- |
| `amj:text`         | `text.js`               | `TextEncoder`, `TextDecoder`                                         |
| `amj:url_impl`     | `url_impl.hpp/.cpp`     | `urlParse`, `urlWithComponent`, `queryStringParse`, `queryStringify` |
| `amj:url`          | `url.js`                | `URL`, `URLSearchParams`                                             |
| `amj:timeout_impl` | `timeout_impl.hpp/.cpp` | `timeoutSet`, `timeoutClear`                                         |
| `amj:timeout`      | `timeout.js`            | `setTimeout`, `clearTimeout`                                         |
| `amj:fetch_impl`   | `fetch_impl.hpp/.cpp`   | `fetchImpl`                                                          |
| `amj:fetch`        | `fetch.js`              | `Headers`, `fetch`                                                   |

`createURLImplModule(jsContext)` registers `amj:url_impl` with four named functions:
`urlParse`, `urlWithComponent`, `queryStringParse`, and `queryStringify`. Each is a
direct QuickJS callback, without magic dispatch or an exported wrapper object.
`urlParse` accepts an omitted/undefined base; invalid URLs and rejected component
updates return null. `url.js` implements coercion, iteration, and live query
synchronization.

`FetchImpl::createModule(jsContext, scheduler)` registers `amj:fetch_impl`.
`fetchImpl.start/read/close` belong to one HTTP owner. `fetch.js` imports that
object and the URL/text classes. JS owns Promises, abort reasons, public argument
normalization, Response objects, and body readers. Response and reader classes
are internal, not public constructors.

`createTimeoutImplModule(jsContext, timeout)` registers `amj:timeout_impl`,
exporting normalized `timeoutSet(callback, delay)` and `timeoutClear(timerID)`
functions. The enclosing host owns one `Timeout` per scheduler/context pair;
its `Scheduler*` borrows the scheduler. Native functions accept `Timeout&`, and
bindings and callbacks retain borrowed `Timeout*` pointers. Keep both objects
at stable addresses: stop JS calls and clear all callbacks before destroying
`Timeout`, and destroy the scheduler before freeing its JS context. The hidden
JS object only carries a borrowed pointer; no native holder allocation or object
finalizer is needed.
The C++ header and `.d.ts` describe those same operations. `timeout.js` implements
the public API: callback validation, numeric coercion, delay clamping/truncation,
and forwarding up to 256 extra arguments via a closure. C++ defensively validates
normalized input and rejects allocation if all positive int32 timer IDs are active.
Cancellation only
accepts IDs belonging to this state's timers, not unrelated scheduler callbacks.
`Timeout` maps its positive int32 timer IDs to native int64 scheduler callback IDs.
Timer IDs wrap from INT32_MAX to 1, skipping active IDs; scheduler IDs never enter
JS. Clearing the scheduler preserves the timer-ID allocation counter. IDs may be
reused after wraparound, so a stale timer ID can then refer to a newer timer.
Registration and the scheduled lambda share a `TimeoutCallback` through `shared_ptr`.
It retains the JS function through RAII; execution removes the
timer ID before invoking JS, and cancellation or scheduler clearing releases both
the callback and its ID. No active flag or separate cleanup registration is needed.
After `scheduler.clear()`, the existing state and module remain usable. All use
and destruction must happen on the owner thread. The context must outlive the
scheduler and retained JS values; borrowed dependencies are guaranteed by the
host's shutdown ordering, not by automatic lifetime extension.

Register each C++ module once per context before loading its consumers. Check
for nullptr and a pending JS exception. Module definitions belong to the context,
not the caller. Each implementation constructs its JS exports inside
its module-registration function. All native instances use the same context and owner thread
as their scheduler.

The embedding host must supply a QuickJS module loader that maps `amj:text`,
`amj:url`, `amj:timeout`, and `amj:fetch` to the installed JS files. Preserve the registered
`*_impl` names, and resolve relative JS imports against their file paths.
There is not yet an application-level JS host/loader in this library; the
module-name mappings in tsconfig configure TypeScript, not QuickJS execution.

After evaluating each public facade, the host can obtain
its namespace with `JS_GetModuleNamespace`, enumerate own string exports, and
copy their values onto its chosen target. No JS installers or hardcoded
constructor-name lists are needed. Check evaluation/property-operation failures
and manage QuickJS value and atom references normally. Decide explicitly whether
existing target properties may be overwritten. Copying exports copies values,
not live bindings; these API exports are not subsequently reassigned.

CMake copies the JS facades, `*_impl.d.ts`, and `global.d.ts` into the build's
`javascript/` directory. Keep these files together when distributing them.

## Types and dependency direction

Each `*_impl.d.ts` describes only its C++ module's JS contract. It never imports
the JS facade. C++ headers retain types only when native code uses them:

- Fetch: `FetchFailure` and `FetchResponseMetadata` hold native completion data.
  `FetchRequestHandle` is a TypeScript-only description of an opaque JS handle.
- URL: `URLParts` and `URLComponent` are TypeScript-only types. Native code
  builds JSONValue maps from Ada URLs and accepts component strings.

`javascript.hpp/.cpp` provides `JSONValue`, an in-memory tree of null, bool,
finite double, string, array, and string-keyed map values. `JSONValue::toJSValue()` converts
it directly to QuickJS values without JSON text serialization. Conversion owns
partial-result cleanup, creates null-prototype objects, and rejects nesting
beyond 128 levels. Map keys are emitted in sorted order.

C++ uses standard pairs/vectors; JS uses inline `[string, string]` tuple/array
types rather than pair aliases. Internal `FetchImplState`, `FetchRequestState`,
and `FetchHandle` are not part of the JS contract. The request handle's TypeScript
brand is type-only; JS must not inspect its representation.

Only `global.d.ts` declares the names installed globally by the host. It references
public JS exports through `typeof import("amj:fetch")` and similar expressions,
including `amj:timeout`. This does not duplicate class or function signatures and
does not execute code. Dependency direction is:

```text
C++ contract <- JS facade <- global.d.ts
```

The `_impl` filename suffix also avoids shadowing inferred JS module types:
`url.d.ts` beside `url.js` would take precedence when resolving imports.
Native declarations are static contracts, not runtime validation; keep them
synchronized with the corresponding headers and implementation.

## Static type checking

JS uses JSDoc and `// @ts-check`, without transpilation. The root `tsconfig.json`
references `global.d.ts` and the three C++ module declarations; `paths` maps public
`amj:*` names to JS implementations. No browser DOM or Node APIs are assumed.
One project checks all JS files, including the implementations, against the
host-provided globals. It retains strict checking, checked indexed access, and
exact optional properties. Run from the repository root with TypeScript 5.2 or newer:

```sh
tsc --project tsconfig.json
```

For future user-script directories, generate a tsconfig with `allowJs`,
`checkJs`, `noEmit`, `lib: ["ES2023"]`, `types: []`, and a `files` list pointing
to installed `global.d.ts`, `url_impl.d.ts`, `fetch_impl.d.ts`, and
`timeout_impl.d.ts`. Add `paths` entries for `amj:text`, `amj:url`, `amj:timeout`,
and `amj:fetch`, pointing to the installed JS files. Paths are relative to the config
or can be generated as absolute installation paths. Editor definitions for globals
lead to `global.d.ts`; following its imported member leads to the JS export.
Following implementation imports leads to the C++ module declarations.
TypeScript cannot navigate into C++.

## Scheduling and cleanup

The outer owner loop drains all QuickJS jobs, checks its awaited Promise, then
calls `scheduler.runNext()` to wait for and execute one native callback. Repeat.
The scheduler does not execute the QuickJS job queue itself. Callback and cleanup
registration accept `std::function<errors::Error()>` values, so callables must be
copyable; pass named callables with `std::move` to avoid copying. Node extraction and map swaps keep callable
movement and destruction outside the mutex, including registration failure paths.

HTTP workers access native buffers only and call `markReady()`. Their request
owners remain alive until explicit close or module cleanup. Scheduler callbacks
hold weak request references, so removing them cannot join a worker under the
scheduler mutex. Buffer locks are released before notifying the scheduler or
calling JS. Completion callback exceptions propagate through `runNext()`.

`fetchImpl.close(request)` joins the worker and drops pending completion functions
without calling them. JS cancellation settles its pending Promises and closes
the handle; scheduler teardown deliberately does not settle abandoned Promises
or run JS. Callers should cancel/release readers in `finally`. Cancellation/join
can block on native DNS/connect/OS calls: do not run this host loop on the UI thread.

Clear/destroy the scheduler before freeing the JS context. `clear()` invalidates
fetch owners, requiring fresh fetch registration/context, but timeout remains
reusable while its borrowed objects remain alive. URL and text do not depend on
scheduler lifetime.

The fetch facade remains a subset: HTTP(S), no redirects/cookies/retries,
string/Uint8Array uploads, `.text()`/`.json()`, and `body.getReader()` streaming.
There are no public Request/Response constructors, cloning, full WHATWG streams,
or body async iteration. Native HTTP validation and memory limits remain enforced
in C++ even though public normalization and API behavior are implemented in JS.
