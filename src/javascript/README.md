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
Fetch is the next module being redesigned.

## Scope

Build the subset needed by provider scripts, not a complete browser runtime.

- Fetch needs ordinary HTTP requests, text/JSON responses, and incremental body
  reads for SSE. Keep reader locking, cancellation, error propagation, and bounded
  unread buffering. No standalone WHATWG Streams module or streaming uploads.
- Follow HTTP(S) redirects by default, resembling browser fetch. Do not preserve
  the old no-redirect policy merely because the existing implementation has it.
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

Use **host-owned native objects, borrowed module bindings, and explicit shutdown**. Pass each module its own dependency, not an unrestricted
pointer to the entire backend host. A module's JS carrier must not delete a
host-owned object.

Host-owned references to JS callbacks/Promise functions are external retained
references. Release them explicitly during shutdown. Do not keep a JS-owned
carrier's cycle-marking arrangement after changing ownership: for a carrier that
only borrows the host-owned object, use `.gc_mark = nullptr`.

QuickJS has no general “shut down all modules” operation. The host calls native
shutdown methods itself. Registering a borrowed dependency does not extend its
lifetime; keep the native object alive through JS finalization.

Console, timeout, and crypto use borrowed bindings. The console sink has no
asynchronous work to shut down; keep it alive until JS teardown finishes.
Legacy fetch still uses scheduler-owned cleanup and is awaiting redesign.

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
   jobs; already-running hashing may continue without accessing JS. The future
   `FetchClient::shutdown()` cancels curl transfers, releases retained JS values,
   and starts closing its libuv socket watchers/timer. Shutdown must be
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
   `unique_ptr<Timeout>`, `unique_ptr<Crypto>`, and future
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

## Fetch redesign

The agreed native boundary exports `fetchClient` from `amj:fetch_impl`.
Its `send(fetchRequest)` returns an opaque `FetchResponse` with response fields,
read operations, and explicit close. Use the full `FetchRequest`, `FetchResponse`,
and `FetchClient` names rather than colliding with browser `Request`/`Response`.
Variables use `fetchRequest`, `fetchResponse`, and `fetchClient`.

`FetchClient` owns the shared curl multi handle and its libuv integration, and
borrows the initialized `uv_loop_t`. Each request owns a curl easy handle and
its request-specific buffers/callbacks. The shared client exists for transport
coordination and connection reuse, not because JS needs numeric request IDs.
One client can drive all HTTP requests on its owner thread. The host controls
whether contexts share it; it is not a process-global variable.

Curl implements HTTP, TLS, redirects, connection reuse, and supported HTTP/2
multiplexing. Libuv watches curl's sockets and deadlines. Use curl multi's socket
API with libuv poll handles and a timer, not blocking HTTP in `uv_queue_work()`
and not a thread per request. Pause/resume receiving for bounded buffering.

Curl callbacks collect native results. Deliver JS completions after the curl
operation unwinds, so JS cancellation or shutdown cannot destroy a transfer
inside curl's callback stack. Finalize the exact asynchronous readiness/read
contract before implementing the new declarations; the existing fetch boundary
is not a compatibility requirement.

### Legacy code still on disk

Fetch currently uses `Scheduler` and cpp-httplib workers; it has not been migrated
to the design above. Timer and digest implementations already use libuv, so the
old scheduler's `runNext()` is not a universal driver for these modules.

Until legacy fetch is removed, clear/destroy its scheduler and join its workers
before freeing QuickJS. The new shutdown sequence does not magically detach
legacy scheduler callbacks. Keep cpp-httplib only where needed for a separate
localhost OAuth callback server, not as the new fetch transport.

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
