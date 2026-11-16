# JavaScript runtime modules

## Implementation order

Keep this order. It is the roadmap for implementing and reviewing modules, not a
claim that every preceding module is finished or reviewed:

1. text
2. url
3. event
4. console
5. timeout
6. abort
7. crypto
8. fetch
9. websocket

URL and timeout are the hand-reviewed references for further module work. Do not
assume the other modules are equally settled just because they already exist.
Fetch now uses curl multi/libuv; websocket remains the next module to implement.

## Review checkpoint

Fetch is implemented but not fully hand-reviewed. Review is proceeding section by section through fetch_impl.hpp/.cpp. Resume at the beginning of settlePromises(): the key snapshot and per-iteration checks protect against same-thread JS reentrancy through jsExecutePendingJobs(), not concurrent access. Explain concrete types, calls, and ownership; don’t treat existing implementation choices as settled requirements.

## Scope

Build the subset needed by provider scripts, not a complete browser runtime.

- Fetch needs ordinary HTTP requests, text/JSON responses, and incremental body
  reads for SSE. Keep reader locking, cancellation, error propagation, and bounded
  unread buffering. No standalone WHATWG Streams module or streaming uploads.
- Follow HTTP(S) redirects by default, resembling browser fetch. The subset does
  not expose manual/error redirect modes, cookie storage, or automatic retries.
- A response-header timeout ends when headers arrive. User/invocation cancellation
  remains effective while consuming the body. Do not impose a fixed total body
  timeout on potentially long-lived SSE connections.
- Chunk sizes are transport details, not response-size limits or SSE message
  boundaries. Bound unread buffering without imposing a lifetime byte limit on
  streaming responses.
- Crypto is limited to random bytes, UUIDs, and SHA digests. No keys, signing,
  encryption, or other SubtleCrypto operations.
- Blob/File, if added, omit `.stream()`.
- Structured cloning, compression streams, and the JS Worker API are out of scope.
  Native worker jobs do not require a JS Worker API.

## Host and module boundary

Facades export values; they do not install globals. The embedding host supplies
module loading and chooses which exports to copy onto its global object. TypeScript
path mappings and `global.d.ts` do not implement either operation. There is not
currently an application-level JS invocation host/loader in this library.

Keep the backend independent of Qt. The intended host has one backend thread
owning `JSRuntime*`, its `JSContext*` objects, and an initialized `uv_loop_t`.
The GUI thread communicates through owned native messages and a queue awakened
with `uv_async_send()`. Never pass JS values or GUI objects across that boundary.
Drain queued messages on wakeup: libuv notifications can coalesce. Keep the
command receiver referenced while accepting commands so an idle backend or
persistent session waits for new commands instead of exiting.

The host shares an `errors::Error` as `pendingError` and checks `hasValue()`
before and after each `uv_run(..., UV_RUN_ONCE)` call. Both a zero error code and
an empty exception pointer mean no error. Callback error reporting assigns only
`errorCode` or `exceptionPtr`, preserving the first error without allocating or
formatting messages. Modules record fatal errors and skip application
work when an error is pending; they do not call `uv_stop()`. Other ready
callbacks can still run within the same iteration, so their entry checks remain
necessary. The host owns error handling and coordinated shutdown. Ordinary
operation failures reject their own Promises rather than failing the runtime.
Completing a tool invocation does not end a persistent session.

Do not nest `uv_run()` to await a JS Promise, or pump Qt and libuv from each
other. The backend performs QuickJS job checkpoints after JS execution/completions
and drains pending jobs before waiting in libuv; pending JS jobs alone do not
wake libuv. The host-provided `jsExecutePendingJobs()` must check both native
`pendingError` and pending QuickJS exceptions before each `JS_ExecutePendingJob()`
call, including the first. If either is present, stop without executing more jobs
and preserve the error for host handling. Handle exceptions reported by
`JS_ExecutePendingJob()` as well. Callers need not duplicate checks whose only
purpose is guarding this job pump.

Network concurrency does not make JavaScript parallel; CPU-bound JS still blocks
its owner thread.

The host must also provide a monotonic `performance.now()` for abort deadlines.
Native `context::Context` does not automatically become a JS `AbortSignal`.
The invocation host must create a JS `AbortController`, expose its signal on the
invocation object, and forward native cancellation/deadlines onto the JS owner
thread. A `std::stop_token` callback must not directly execute QuickJS from an
arbitrary cancellation thread.

## Ownership decision

Use **host-owned native objects, borrowed module bindings, and explicit shutdown**.
Pass each module its own dependency, not an unrestricted pointer to the entire
backend host. A module's JS carrier must not delete a
host-owned object.

Host-owned references to JS callbacks/Promise functions are external retained
references. Release them explicitly during shutdown. Do not keep a JS-owned
carrier's cycle-marking arrangement after changing ownership: for a carrier that
only borrows the host-owned object, use `.gc_mark = nullptr`.

QuickJS has no general “shut down all modules” operation. The host calls native
shutdown methods itself. Registering a borrowed dependency does not extend its
lifetime; keep the native object alive through JS finalization.

Console, timeout, crypto, and fetch use borrowed bindings. The console sink has
no asynchronous work to shut down; keep it alive until JS teardown finishes.

## Shutdown contract

This is the shutdown contract for the host-owned implementation. All steps run
on the thread owning the `JSRuntime*`, `JSContext*`, and `uv_loop_t`.

1. **Stop executing application JavaScript.** Stop accepting scripts/commands
   that run JS. Finish the current callback before beginning teardown. Do not
   subsequently use `JS_Eval()`, `JS_Call()`, or `JS_ExecutePendingJob()` to run
   application code. Native completion hooks must stop delivering JS too.
2. **Call native shutdown methods while QuickJS is alive.** For example,
   `Timeout::shutdown()` closes its `uv_timer_t` handles and releases timer
   callbacks. `Crypto::shutdown()` releases JS callbacks and detaches worker
   jobs; already-running hashing may continue without accessing JS.
   `FetchClient::shutdown()` cancels curl transfers, releases retained JS values,
   and starts closing its libuv socket watchers, curl deadline timer, and delivery
   `uv_async_t` handle. Shutdown must be
   idempotent and must not execute JS or disturb pending JS exceptions.
3. **Finish native asynchronous cleanup.** Keep driving the `uv_loop_t` with
   `uv_run(..., UV_RUN_ONCE)` even when `pendingError` is set, so `uv_close()`
   callbacks can free handle storage and outstanding digest workers can deliver
   native-only completion cleanup. Close the host's `uv_async_t` command receiver
   and other persistent handles too; otherwise the loop remains alive and draining
   cannot finish. Storage must remain valid until the last callback using it
   finishes. Do not execute application JS during this drain.
4. **Destroy QuickJS.** Release remaining host-owned JS values, call
   `JS_FreeContext()` for every `JSContext*`, then `JS_FreeRuntime()` for the
   `JSRuntime*`. Native objects borrowed by JS carriers must still exist because
   JS finalizers can run during this step.
5. **Destroy the native C++ objects.** For example, reset the host's
   `unique_ptr<Timeout>`, `unique_ptr<Crypto>`, and
   `unique_ptr<FetchClient>`. They have already been shut down: their destructors
   must not access the freed `JSContext*` or `JSRuntime*`, including through a
   repeated `shutdown()` call.
6. **Close the libuv loop.** Call `uv_loop_close()` on the `uv_loop_t*` and check
   the result. `UV_EBUSY` means cleanup is incomplete; do not free the loop's
   storage. Free dynamically allocated loop storage only after closure succeeds.

Ordinary cancellation and host teardown are different. During ordinary operation,
canceling a fetch must settle pending header/read Promises. During host teardown,
release retained Promise functions without invoking them; abandoned Promises do
not need to settle.

## Fetch ownership and transport

Keep three responsibilities separate: `FetchRequest` is copied input,
`FetchResponse` is the lightweight JS-owned handle, and `FetchState` is native
operation storage owned by `FetchClient`. The response borrows the client and
identifies its state by easy handle. The state has no client back-pointer.
Do not merge the state into the response and recreate mutually referential
client/response types.

Closing a response invalidates its own easy-handle key before destroying its
client-owned state. This prevents stale JS objects from addressing a new request
if curl reuses an address. Network completion alone does not erase the state:
unread buffered bytes must remain available. Callers must consume or cancel
bodies, preferably in `finally`; merely releasing a reader lock is not cancellation.
JS finalization is fallback cleanup, not the host's shutdown mechanism. Finalizers
mark state for deferred cleanup: delivery allocations can trigger GC, so a finalizer
must not synchronously erase state being used by delivery or execute JS to settle
Promises. After client shutdown, finalizers must not look up stale easy-handle keys.

The client shares connections across operations, not numeric request IDs with JS.
It is currently bound to one `JSContext*` and initialized `uv_loop_t`, not a
process-global variable. Curl implements HTTP, TLS, redirects, connection reuse,
and supported HTTP/2 multiplexing; libuv watches curl's sockets and deadlines.
Do not replace this with blocking HTTP in `uv_queue_work()` or a thread per request.
Keep cpp-httplib only where needed for a separate localhost OAuth callback server.

Curl callbacks only collect native results. A separate `uv_async_t` notification
settles native-operation Promises after curl unwinds; otherwise Promise settlement
could reenter JS and cancel a transfer inside curl's callback stack. Scheduling
only signals the handle; the constructor wires its callback to delivery, avoiding
circular helper dependencies. The handle is referenced while delivery is pending
and unreferenced when that notification is consumed, so an idle client does not
keep the loop running. Curl's deadline timer remains a `uv_timer_t`. Delivery drops map
references before host job checkpoints because those jobs may close responses or
shut down the client. Pause receiving when buffers fill, and enqueue nothing when
returning curl's pause result: curl redelivers that entire block on resume.

The public facade preserves the original JS abort reason, rather than exposing
native close's generic rejection. Native readiness ends at the final response's
headers, not at an informational/redirect header block. No fixed body deadline
is imposed. An invocation host must run libuv to drive fetch, timers, and digests.

## Unresolved curl cleanup error handling (handoff)

**This is an unresolved safety/design issue, not an implemented recovery plan.**
Do not replace cleanup-time `std::terminate()` with a `pendingError` assignment
and claim that cleanup is now safe. Do not claim that returning an error or
retrying on another event-loop iteration guarantees eventual cleanup either.

### Current code and user intent

Fetch currently uses raw owned `CURL*` and `CURLM*` fields:

- `FetchState::ownedCurlEasyHandle` owns the easy handle.
- `FetchState::curlMultiHandle` borrows the multi handle while registered.
- `FetchClient::ownedCurlMultiHandle` owns the multi handle.
- A local `std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>` protects a newly
  created, unregistered easy handle in `FetchClient::send()`, then releases
  ownership to its `FetchState`.

The experimental `owned::CurlEasyHandle` and `owned::CurlMultiHandle` wrappers
were removed, including `src/owned/curl.hpp`, `.cpp`, and their CMake entry.
The user prefers raw curl calls and does not want registration APIs hidden by
wrapper methods. Returning to raw handles simplifies the location of ownership
coordination; it DOES NOT solve cleanup failures. Do not reintroduce wrappers
as though that resolves this issue.

The current fail-fast behavior predates those wrappers and remains unresolved:

- `FetchState::~FetchState()` calls `curl_multi_remove_handle()` when tracked as
  registered and terminates on any non-`CURLM_OK` result. It then cleans up the
  easy handle and frees request headers.
- `FetchClient::shutdown()` sets `shuttingDown`, clears `fetchStateMap`, calls
  `curl_multi_cleanup()`, and terminates on any non-`CURLM_OK` result. It then
  releases client callback data and closes libuv handles.
- The constructor catches failures, calls `shutdown()`, and rethrows. The
  destructor also calls `shutdown()`.

The user objects to unconditional process termination on curl cleanup errors.
They are willing to use explicit, fallible cleanup rather than destructor-driven
cleanup. This direction was discussed but HAS NOT been implemented. No complete
recovery strategy for all cleanup failures has been established.

### What was actually checked

The installed header at
`build/debug/vcpkg_installed/arm64-osx/include/curl/curlver.h` reported libcurl
**8.22.0** during this investigation. Recheck the version before relying on
implementation details. Sources inspected:

- https://curl.se/libcurl/c/curl_multi_remove_handle.html
- https://curl.se/libcurl/c/curl_multi_cleanup.html
- https://curl.se/libcurl/c/curl_easy_cleanup.html
- https://github.com/curl/curl/blob/curl-8_22_0/lib/multi.c

The public docs require removing easy handles before easy cleanup, then cleaning
up the multi handle after its easy handles are removed. Removal is allowed
mid-transfer, but not from inside a libcurl callback. Multi cleanup may invoke
socket callbacks while shutting down its connection pool, so callback data and
its dependencies must remain valid until cleanup returns.

The public removal documentation describes success/error return codes but does
not provide a blanket transactional guarantee about registration state after
an error. In the inspected 8.22.0 implementation:

- Removing an already unregistered easy handle returns `CURLM_OK`.
- Wrong-multi and internal-consistency checks can reject removal BEFORE
  detachment. API entry guards can also reject prohibited calls.
- Removal clears the easy handle's multi association and removes it from the
  multi's transfer tables BEFORE calling `Curl_update_timer()`.
- `Curl_update_timer()` can invoke the application's timer callback. If that
  returns `-1`, it returns `CURLM_ABORTED_BY_CALLBACK` AFTER detachment has
  already happened. There is also a later wakeup-assessment failure path.

Therefore, **a non-OK removal result does not universally mean the handle is
still registered**. Conversely, it does not universally mean detachment
succeeded. The existing termination comment ("Never destroy callback storage
still registered with curl") assumes too much when applied to every error.
A callback failure after successful detachment can currently cause needless
termination. Any proposed classification of error codes must be verified
against all relevant paths and supported versions; do not assume the inspected
implementation is a documented cross-version API guarantee.

In the inspected `curl_multi_cleanup()` implementation, the failure paths we
identified concern invalid handles, prohibited API entry, or invalid internal
handle state, rather than ordinary HTTP/network errors. Normal successful
cleanup frees the multi allocation. We have NOT established a supported recovery
procedure for every non-OK result, nor a general post-error ownership guarantee.
Do not assume that another loop iteration or retry fixes invalid/corrupt state.

### Why the existing pendingError path is different

In `settlePendingPromises()`, failure to resume a paused transfer leads to an
explicit removal attempt. A removal error preserves the first `pendingError`
and returns WITHOUT destroying the state. This is different from reporting an
error in a destructor and then allowing all callback storage to be destroyed.
That path currently leaves `fetchState.curlMultiHandle` unchanged on ALL removal
errors, even errors that the inspected implementation can return after
successful detachment. Its registration bookkeeping also needs examination.

There are existing checks of `pendingError` in `driveCurlTransfers()`, promise
delivery, and callback/API entry points. These stop application work, not native
ownership/destruction. The host contract elsewhere in this document requires
checking before/after `uv_run()`, stopping JS work, and coordinating shutdown.
During this investigation, no concrete host-loop implementation handling this
fetch error recovery was found elsewhere in `src`. Do not describe the host
contract as an already implemented cleanup-recovery mechanism.

Setting `pendingError` is error reporting, NOT a proof that resources can now be
freed. It also does not prevent the host from subsequently entering the current
fail-fast teardown path. Preserve the first error rather than overwriting it
with a secondary cleanup failure.

### What a fallible cleanup design would need

These are requirements to investigate, not a promise that all failures are
recoverable:

1. Perform fallible deregistration BEFORE destructive map erasure. Audit all
   state-erasure paths, not only shutdown: response close, deferred GC cleanup,
   send failures, and exception paths. Also audit add-handle failure semantics
   rather than assuming failure always means no registration occurred.
2. Establish when detachment has occurred. Track successful removal and any
   specifically verified post-detachment error outcomes accurately. Do not
   blindly clear registration on every failure or retain stale tracking forever.
3. On an error for which safe detachment cannot be established, return control
   before destroying the affected state, easy handle, headers, request body,
   callback wrapper, or any other borrowed dependencies.
4. Make client shutdown staged and explicitly fallible. Distinguish stopping new
   application work from completion of native cleanup. The current
   `shuttingDown` early return cannot represent partial cleanup and retries.
5. Only release multi callback data, poll watchers, deadline timer, delivery
   handle, and their dependencies at a point where curl can no longer use them.
   Multi cleanup itself can call back into the client. Account for callbacks
   recording errors even when the curl cleanup return value is successful.
6. Keep the client, QuickJS context/runtime where still referenced, libuv loop,
   pending-error storage, and other borrowed dependencies alive on incomplete
   cleanup. Returning an error from a destructor or constructor catch does not
   stop C++ member destruction. Constructor failure therefore needs separate
   consideration; an incompletely constructed client cannot simply be retained
   by the caller as a successfully constructed object.
7. Decide how explicit cleanup results reach the host, including failures during
   an already-in-progress shutdown. A result code and/or `pendingError` can
   report failure, but neither provides recovery by itself. Do not pump the
   event loop recursively from a curl callback to attempt recovery.
8. Define a terminal policy when safe cleanup cannot be completed. Retaining
   storage is only a temporary solution unless its eventual disposition is
   defined. Leaking only a handle while destroying its callback dependencies is
   not a safe retention strategy. Retrying blindly is not a recovery policy.

### What remains unknown / must not be promised

Explicit fallible cleanup CAN provide error propagation and a decision point
before storage destruction. We do NOT yet know a general, guaranteed way to
reclaim every resource safely after every possible curl cleanup failure.

A prohibited recursive call can potentially be deferred until outside the
callback. A specifically verified post-detachment callback error can potentially
be reported while proceeding with easy cleanup. Invalid handles or inconsistent
internal state have no established supported recovery procedure in this work.
These categories must not be conflated.

Before implementation, settle the supported libcurl guarantees, the error
classification, the host ownership protocol, constructor-failure handling, and
the final unrecoverable-error policy. The previous discussion overstated the
benefit of raw handles and of merely introducing fallible shutdown; neither
alone makes failed cleanup safe. Ask the user about unresolved policy choices
rather than silently choosing termination, unsafe destruction, or indefinite
retention.

## Declaration files and checking

Keep `*_impl.d.ts` independent of public facades: it describes the native/JS
boundary, not browser APIs. `global.d.ts` references public exports without
installing them. Prefer inferred facade types unless an explicit declaration is
needed to prevent advertising unsupported APIs, as with `crypto.d.ts`.

Check JS and declarations from the repository root:

```sh
tsc --project tsconfig.json
```

CMake copies runtime JS and declaration files into the build's `javascript/`
directory. Update its distribution list and TypeScript module mappings when
adding or removing modules; keep installed files together.
