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
necessary. The host owns error handling and coordinated teardown. Ordinary
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

Use **host-owned native objects, borrowed module bindings, and explicit cleanup**.
Pass each module its own dependency, not an unrestricted pointer to the entire
backend host. A module's JS carrier must not delete a
host-owned object.

Host-owned references to JS callbacks/Promise functions are external retained
references. Release them explicitly during cleanup. Do not keep a JS-owned
carrier's cycle-marking arrangement after changing ownership: for a carrier that
only borrows the host-owned object, use `.gc_mark = nullptr`.

QuickJS has no general module-cleanup operation. The host calls native cleanup
methods itself. Registering a borrowed dependency does not extend its
lifetime; keep the native object alive through JS finalization.

Console, timeout, crypto, and fetch use borrowed bindings. The console sink has
no asynchronous work to close; keep it alive until JS teardown finishes.

## Cleanup contract

This is the cleanup contract for the host-owned implementation. All steps run
on the thread owning the `JSRuntime*`, `JSContext*`, and `uv_loop_t`.

1. **Stop executing application JavaScript.** Stop accepting scripts/commands
   that run JS. Finish the current callback before beginning teardown. Do not
   subsequently use `JS_Eval()`, `JS_Call()`, or `JS_ExecutePendingJob()` to run
   application code. Native completion hooks must stop delivering JS too.
2. **Call native cleanup methods while QuickJS is alive.** For example,
   `Timeout::shutdown()` closes its `uv_timer_t` handles and releases timer
   callbacks. `Crypto::shutdown()` releases JS callbacks and detaches worker
   jobs; already-running hashing may continue without accessing JS.
   `FetchClient::close()` cancels curl transfers, releases retained JS values,
   and starts closing its libuv socket watchers, curl deadline timer, and delivery
   `uv_async_t` handle. Inspect its returned error without overwriting an existing
   `pendingError`. Cleanup must be idempotent and must not execute JS or disturb
   pending JS exceptions.
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
   `unique_ptr<FetchClient>`. They have already been cleaned up: their destructors
   must not access the freed `JSContext*` or `JSRuntime*`, including through repeated
   cleanup calls (`close()` for fetch, `shutdown()` for timeout and crypto).
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
JS finalization is fallback cleanup, not the host's explicit cleanup mechanism. Finalizers
mark state for deferred cleanup: delivery allocations can trigger GC, so a finalizer
must not synchronously erase state being used by delivery or execute JS to settle
Promises. After client close(), finalizers must not look up stale easy-handle keys.

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
close the client. Pause receiving when buffers fill, and enqueue nothing when
returning curl's pause result: curl redelivers that entire block on resume.

The public facade preserves the original JS abort reason, rather than exposing
native close's generic rejection. Native readiness ends at the final response's
headers, not at an informational/redirect header block. No fixed body deadline
is imposed. An invocation host must run libuv to drive fetch, timers, and digests.

## Curl cleanup contract

Fetch uses raw curl handles: `FetchState::ownedCurlEasyHandle` owns an easy
handle, `FetchState::curlMultiHandle` borrows its multi handle while registered,
and `FetchClient::ownedCurlMultiHandle` owns the multi handle. Keep curl calls
visible rather than hiding registration behind additional ownership wrappers.

`FetchState::close()` and `FetchClient::close()` are idempotent, nonthrowing,
and return `errors::Error`. A normal return means synchronous cleanup completed,
including when an operational error is returned. It does not mean libuv close
callbacks have finished; the host must still drain the loop. Programmer/API
contract errors and libcurl internal failures terminate.

Call state `close()` explicitly before map erasure or extracted-node destruction
and preserve the first `pendingError`. Response close and deferred abandoned-state
cleanup move retained Promise functions into locals before closing the state,
then reject them with AbortError if no native error is pending. This makes all
subsequent early returns and exceptions safe without relying on destructor cleanup.
Destructors call `close()` as fallback and discard its returned error.
The client constructor also closes partially initialized resources before rethrowing.

The client closes every state, clears its map, then calls `curl_multi_cleanup()`.
Keep callback data, socket watchers, timer, and their dependencies alive until
multi cleanup returns: it can synchronously invoke socket callbacks. Prefer a
callback's original `pendingError` over curl's generic callback-failure code.
Client `closed` guards repeated/reentrant closure; it does not describe a partial
cleanup or retry protocol. Do not resume application JS after client close().

### Verified libcurl behavior

The cleanup classification was checked against libcurl **8.22.0**:

- https://curl.se/libcurl/c/curl_multi_remove_handle.html
- https://curl.se/libcurl/c/curl_multi_cleanup.html
- https://curl.se/libcurl/c/curl_easy_cleanup.html
- https://github.com/curl/curl/blob/curl-8_22_0/lib/api.c
- https://github.com/curl/curl/blob/curl-8_22_0/lib/multi.c
- https://github.com/curl/curl/blob/curl-8_22_0/lib/multi_ev.c

Removal's timer-update and wakeup-assessment paths run after detachment. Their
`CURLM_ABORTED_BY_CALLBACK` and `CURLM_OUT_OF_MEMORY` results are reported while
easy cleanup proceeds. Other removal failures are fatal under the chosen policy.
Multi cleanup's non-OK results are programmer/internal failures and terminate;
callbacks can still record operational errors even when its result is `CURLM_OK`.
Add-handle failures in the inspected implementation occur before registration or
undo the association before returning; send failure paths close the unregistered state.
These are implementation-specific facts, not blanket cross-version guarantees;
recheck the classification when changing supported libcurl versions.

The separate paused-transfer resume-failure path still removes the easy handle
without destroying its state, because response error delivery still needs the
retained Promises. Its registration bookkeeping on removal errors remains a
review item; do not replace that operation with full state `close()`.

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
