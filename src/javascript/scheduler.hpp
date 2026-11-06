#pragma once

#include "errors/errors.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>

struct JSContext;

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;

enum class ErrorCode : std::uint8_t {
    Stalled = 1,
    JSException,
};

[[nodiscard]] std::error_code make_error_code(ErrorCode errorCode);

// All methods except markReady() require the constructing thread. Callbacks and
// cleanups execute outside the mutex. Registered callables must be copyable;
// pass named callables with std::move to avoid copying. Captures are moved and
// destroyed on that thread,
// outside the mutex. During clear(), registration and runNext() are
// rejected; nested clear() is a no-op. Workers must stop/join before the scheduler
// is destroyed; registered cleanups may do this. The borrowed context must outlive
// the scheduler.
//
// Supplied functions return zero-code Error for success. Pending JS exceptions
// after callbacks execute are reported as JSException. Error codes
// returned by supplied functions are not validated against the JS exception state.
// Unexpected C++ exceptions from supplied functions become GenericError.
// Allocation/synchronization failures in scheduler operations may still throw.
class Scheduler {
  public:
    explicit Scheduler(JSContext* jsContext);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // nullopt means readiness is explicit. An elapsed callAt or markReady() makes the
    // callback runnable. IDs are never reused, including across clear().
    [[nodiscard]] std::expected<int64_t, errors::Error> registerCallback(std::function<errors::Error()> callback, std::optional<chrono::steady_clock::time_point> optionalTriggerTime);

    // Removes the callback and its readiness timestamp/deadline; invokes no cleanup.
    // Unknown IDs are ignored.
    [[nodiscard]] errors::Error unregisterCallback(int64_t callbackID);

    // Thread-safe. Records when first marked ready and wakes runNext(). Repeated
    // calls coalesce; unknown/removed IDs are ignored. Never executes JS or
    // accesses the JS context.
    [[nodiscard]] errors::Error markReady(int64_t callbackID);

    // Cleanups only cancel native operations and release resources. They must not
    // execute JS or modify the pending JS exception; JS cleanup belongs in finally.
    [[nodiscard]] std::expected<int64_t, errors::Error> registerCleanup(std::function<errors::Error()> cleanup);

    // Removes before invoking, so repeat/reentrant calls are no-ops. Does not
    // unregister callbacks: modules manage those independently.
    [[nodiscard]] errors::Error cleanup(int64_t cleanupID);

    // Waits for and executes one callback, choosing the earliest readiness
    // timestamp/elapsed deadline, then ID. Removes it before invocation and runs
    // it outside the mutex. No callbacks means Stalled. The caller must drain
    // pending JS jobs and check its awaited Promise before each call.
    [[nodiscard]] errors::Error runNext();

    // Removes all callbacks/cleanups, attempts every cleanup, returns the first
    // cleanup failure. Leaves pending JS exceptions untouched. Remains reusable.
    // Destructor calls clear() and terminates if it fails or throws.
    [[nodiscard]] errors::Error clear();

  private:
    JSContext* jsContext = nullptr;
    std::thread::id ownerThreadID = {};
    std::condition_variable conditionVariable = {};
    // The mutex protects all fields below it. Callbacks and cleanups execute
    // outside the lock. Fields above it are construction-only or self-synchronized.
    std::mutex mutex = {};
    std::map<int64_t, std::function<errors::Error()>> callbackMap = {};
    std::map<int64_t, chrono::steady_clock::time_point> triggerTimeMap = {};
    std::map<int64_t, chrono::steady_clock::time_point> readyTimeMap = {};
    std::map<int64_t, std::function<errors::Error()>> cleanupMap = {};
    int64_t callbackIDCounter = 0;
    int64_t cleanupIDCounter = 0;
    bool clearing = false;
};

} // namespace amj::javascript

namespace std {

template <>
struct is_error_code_enum<amj::javascript::ErrorCode> : true_type {};

} // namespace std
