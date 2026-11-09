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
Drain queued messages on wakeup: libuv notifications can coalesce.

Do not nest `uv_run()` to await a JS Promise, or pump Qt and libuv from each
other. The backend runs libuv and performs QuickJS job checkpoints after JS
execution/completions. Network concurrency does not make JavaScript parallel;
CPU-bound JS still blocks its owner thread.

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
   `uv_run()` so `uv_close()` callbacks can free handle storage and outstanding
   digest workers can deliver native-only completion cleanup. Close the host's
   `uv_async_t` command receiver and other persistent handles too; otherwise
   `UV_RUN_DEFAULT` may never return. Storage must remain valid until the last
   callback using it finishes. Do not execute application JS during this drain.
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
is imposed. `Scheduler` remains on disk but no longer drives fetch, timers, or
digests; an invocation host must run libuv, not `Scheduler::runNext()`.

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
