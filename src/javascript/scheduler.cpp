#include "scheduler.hpp"

#include "errors/errors.hpp"

#include <limits>
#include <quickjs.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace this_thread = std::this_thread;
namespace {

class ErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "amj.javascript";
    }

    std::string message(int value) const override {
        switch (value) {
            case static_cast<int>(ErrorCode::Stalled):
                return "No scheduled callback can make progress";
            case static_cast<int>(ErrorCode::JSException):
                return "JavaScript exception pending in context";
            default:
                return "Unknown JavaScript error";
        }
    }
};

} // namespace

std::error_code make_error_code(ErrorCode errorCode) {
    static const ErrorCategory category;
    return std::error_code(static_cast<int>(errorCode), category);
}

Scheduler::Scheduler(JSContext* jsContext) {
    if (jsContext == nullptr) {
        throw std::invalid_argument("Scheduler.constructor: jsContext is nullptr");
    }
    this->jsContext = jsContext;
    this->ownerThreadID = this_thread::get_id();
}

Scheduler::~Scheduler() {
    try {
        const errors::Error error = clear();
        if (error.errorCode) {
            std::terminate();
        }
    } catch (...) {
        std::terminate();
    }
}

std::expected<int64_t, errors::Error> Scheduler::registerCallback(std::function<errors::Error()> callback, std::optional<chrono::steady_clock::time_point> triggerTime) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::message("Scheduler.registerCallback: called outside owner thread"));
    }
    // Prepare the node before locking: moving captures can execute user code.
    std::map<int64_t, std::function<errors::Error()>> temporaryMap;
    temporaryMap.insert_or_assign(0, std::move(callback));
    std::map<int64_t, std::function<errors::Error()>>::node_type mapNode = temporaryMap.extract(0);
    const std::lock_guard<std::mutex> lock(mutex);
    if (clearing) {
        return std::unexpected(errors::message("Scheduler.registerCallback: called inside a clear()"));
    }
    if (mapNode.mapped() == nullptr) {
        return std::unexpected(errors::message("Scheduler.registerCallback: registered a nullptr callback function"));
    }
    if (callbackIDCounter >= std::numeric_limits<int64_t>::max()) {
        return std::unexpected(errors::message("Scheduler.registerCallback: callbackIDs exhausted"));
    }
    callbackIDCounter++;
    const int64_t callbackID = callbackIDCounter;
    mapNode.key() = callbackID;
    if (triggerTime.has_value()) {
        triggerTimeMap.insert_or_assign(callbackID, triggerTime.value());
    }
    // Inserting a prepared node does not allocate or move the callback.
    callbackMap.insert(std::move(mapNode));
    return callbackID;
}

errors::Error Scheduler::unregisterCallback(int64_t callbackID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("Scheduler.unregisterCallback: called outside owner thread");
    }
    // Lift the callback out into the parent scope and not the mutex scope so
    // that values captured by the callback don't run their destructors under
    // the mutex scope.
    std::map<int64_t, std::function<errors::Error()>>::node_type mapNode;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        mapNode = callbackMap.extract(callbackID);
        triggerTimeMap.erase(callbackID);
        readyTimeMap.erase(callbackID);
    }
    // Destroy captures only after unlocking.
    return {};
}

errors::Error Scheduler::markReady(int64_t callbackID) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!callbackMap.contains(callbackID)) {
            return {};
        }
        if (readyTimeMap.contains(callbackID)) {
            return {};
        }
        readyTimeMap.insert_or_assign(callbackID, chrono::steady_clock::now());
    }
    conditionVariable.notify_one();
    return {};
}

std::expected<int64_t, errors::Error> Scheduler::registerCleanup(std::function<errors::Error()> cleanup) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::message("Scheduler.registerCleanup: called outside owner thread"));
    }
    std::map<int64_t, std::function<errors::Error()>> temporaryMap;
    temporaryMap.insert_or_assign(0, std::move(cleanup));
    std::map<int64_t, std::function<errors::Error()>>::node_type mapNode = temporaryMap.extract(0);
    const std::lock_guard<std::mutex> lock(mutex);
    if (clearing) {
        return std::unexpected(errors::message("Scheduler.registerCleanup: called inside a clear()"));
    }
    if (mapNode.mapped() == nullptr) {
        return std::unexpected(errors::message("Scheduler.registerCleanup: registered a nullptr cleanup function"));
    }
    if (cleanupIDCounter >= std::numeric_limits<int64_t>::max()) {
        return std::unexpected(errors::message("Scheduler.registerCleanup: cleanupIDs exhausted"));
    }
    cleanupIDCounter++;
    const int64_t cleanupID = cleanupIDCounter;
    mapNode.key() = cleanupID;
    cleanupMap.insert(std::move(mapNode));
    return cleanupID;
}

errors::Error Scheduler::cleanup(int64_t cleanupID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("Scheduler.cleanup: called outside owner thread");
    }
    std::map<int64_t, std::function<errors::Error()>>::node_type mapNode;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        mapNode = cleanupMap.extract(cleanupID);
    }
    if (mapNode.empty()) {
        return {};
    }
    errors::Error error;
    try {
        error = mapNode.mapped()();
    } catch (const std::exception& exception) {
        error = errors::message(std::string("Scheduler.cleanup: cleanup threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::message("Scheduler.cleanup: cleanup threw an unknown C++ exception");
    }
    return error;
}

errors::Error Scheduler::runNext() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("Scheduler.runNext: called outside owner thread");
    }
    std::map<int64_t, std::function<errors::Error()>>::node_type mapNode;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (clearing) {
            return errors::message("Scheduler.runNext: called inside a clear()");
        }
        if (JS_HasException(jsContext)) {
            return errors::message("Scheduler.runNext: cannot run next callback with a pending JavaScript exception");
        }
        while (true) {
            if (callbackMap.empty()) {
                return errors::Error{
                    .errorCode = ErrorCode::Stalled,
                    .errorMessage = "Scheduler.runNext: No scheduled callback can make progress; invocation is stalled",
                };
            }
            int64_t callbackID = 0;
            std::optional<chrono::steady_clock::time_point> timePoint = {};
            for (const std::pair<const int64_t, std::function<errors::Error()>>& entry : callbackMap) {
                // Check the readyTimeMap.
                if (readyTimeMap.contains(entry.first)) {
                    const chrono::steady_clock::time_point readyTime = readyTimeMap.at(entry.first);
                    if (!timePoint.has_value() || readyTime < timePoint.value()) {
                        callbackID = entry.first;
                        timePoint = readyTime;
                    }
                }
                // Check the triggerTimeMap.
                if (triggerTimeMap.contains(entry.first)) {
                    const chrono::steady_clock::time_point triggerTime = triggerTimeMap.at(entry.first);
                    if (!timePoint.has_value() || triggerTime < timePoint.value()) {
                        callbackID = entry.first;
                        timePoint = triggerTime;
                    }
                }
            }
            if (timePoint.has_value()) {
                if (timePoint.value() <= chrono::steady_clock::now()) {
                    mapNode = callbackMap.extract(callbackID);
                    triggerTimeMap.erase(callbackID);
                    readyTimeMap.erase(callbackID);
                    break;
                }
                conditionVariable.wait_until(lock, timePoint.value());
            } else {
                conditionVariable.wait(lock);
            }
        }
    }
    errors::Error error = {};
    try {
        error = mapNode.mapped()();
    } catch (const std::exception& exception) {
        error = errors::message(std::string("Scheduler.runNext: callback threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::message("Scheduler.runNext: callback threw an unknown C++ exception");
    }
    if (JS_HasException(jsContext)) {
        error.errorCode = ErrorCode::JSException;
    }
    return error;
}

errors::Error Scheduler::clear() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("Scheduler.clear: called outside owner thread");
    }
    std::map<int64_t, std::function<errors::Error()>> temporaryCallbackMap;
    std::map<int64_t, std::function<errors::Error()>> temporaryCleanupMap;
    {
        // Clear the callback maps under mutex lock without triggering any
        // destructors by swapping temporary empty maps with the current ones.
        // So other callers of callbackMap and cleanupMap will see it as empty
        // (same effect as clear()) while the actual maps are now owned in this
        // function's scope and can be destroyed safely outside the mutex lock
        // on function return.
        const std::lock_guard<std::mutex> lock(mutex);
        if (clearing) {
            return {};
        }
        clearing = true;
        temporaryCallbackMap.swap(callbackMap);
        triggerTimeMap.clear();
        readyTimeMap.clear();
        temporaryCleanupMap.swap(cleanupMap);
    }
    temporaryCallbackMap.clear();
    errors::Error firstError;
    for (const std::pair<const int64_t, std::function<errors::Error()>>& entry : temporaryCleanupMap) {
        errors::Error error;
        try {
            error = entry.second();
        } catch (const std::exception& exception) {
            error = errors::message(std::string("Scheduler.clear: cleanup threw a C++ exception: ") + exception.what());
        } catch (...) {
            error = errors::message("Scheduler.clear: cleanup threw an unknown C++ exception");
        }
        if (!firstError.errorCode && error.errorCode) {
            firstError = error;
        }
    }
    temporaryCleanupMap.clear();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        clearing = false;
    }
    return firstError;
}

} // namespace amj::javascript
