#include "timeout_impl.hpp"

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "scheduler.hpp"

#include <quickjs.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

std::mutex timeoutClassIDMutex = {};
std::map<JSRuntime*, JSClassID> timeoutClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

// Owned by the scheduler callback. Destruction handles cancellation and clear()
// without reentering the scheduler.
struct TimeoutCallback {
    JSContext* jsContext = nullptr;
    JSValue callback = JS_UNDEFINED;
    Timeout* timeout = nullptr; // Owner keeps this alive through callback destruction.
    int32_t timerID = 0;
    int64_t callbackID = 0;

    ~TimeoutCallback() {
        try {
            // A timer ID may have been reused while this callback was executing.
            if (timeout != nullptr && timeout->registry.contains(timerID) && timeout->registry.at(timerID) == callbackID) {
                timeout->registry.erase(timerID);
            }
            JS_FreeValue(jsContext, callback);
        } catch (...) {
            std::terminate();
        }
    }

    errors::Error call() {
        if (timeout->registry.contains(timerID) && timeout->registry.at(timerID) == callbackID) {
            timeout->registry.erase(timerID);
        }
        const ScopedJSValue result(jsContext, JS_Call(jsContext, callback, JS_UNDEFINED, 0, nullptr));
        if (JS_IsException(result.value)) {
            return errors::Error{
                .errorCode = ErrorCode::JSException,
                .errorMessage = "Timer callback threw a JavaScript exception",
            };
        }
        return {};
    }
};

} // namespace

JSValue timeoutSet(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, Timeout& timeout) {
    try {
        if (timeout.scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: missing scheduler");
        }
        if (argc < 2) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: requires at least 2 arguments (%d provided)", argc);
        }

        // Get callback function.
        if (!JS_IsFunction(jsContext, argv[0])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg0: callback not a function");
        }
        ScopedJSValue callback(jsContext, JS_DupValue(jsContext, argv[0]));

        // Get delayMs number.
        if (!JS_IsNumber(argv[1])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg1: delay not a number");
        }
        double delayMs = 0.0;
        const int status = JS_ToFloat64(jsContext, &delayMs, argv[1]);
        if (status < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(delayMs) || std::floor(delayMs) != delayMs || delayMs < 0.0 || delayMs > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: arg1: delay must be a positive number within 32-bit signed integer range");
        }

        // Ensure now + delay is within chrono::steady_clock::time_point's
        // maximum range.
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        const chrono::milliseconds delayDuration(static_cast<int64_t>(delayMs));
        if (now > chrono::steady_clock::time_point::max() - delayDuration) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: timer deadline is out of range");
        }

        // Increment timerIDCounter to the next available timerID. timerIDs can
        // be reused (the counter wraps around starting from 1), so after
        // wrapping around it's possible that the next timerID is still in use
        // (check the timerCallbackIDs map) so we have to keep incrementing
        // until we find one that is not in use.
        while (true) {
            // If the timerCallbackIDs map is full, there are no more available
            // timerIDs.
            if (timeout.registry.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                return JS_ThrowRangeError(jsContext, "timeoutSet: timerIDs exhausted");
            }
            // Increment the timerIDCounter by 1, wrapping when we hit
            // int32_t's maximum value.
            if (timeout.timerIDCounter == std::numeric_limits<int32_t>::max()) {
                timeout.timerIDCounter = 1;
            } else {
                timeout.timerIDCounter++;
            }
            // If the timerID is in use, try again.
            if (timeout.registry.contains(timeout.timerIDCounter)) {
                continue;
            }
            break;
        }
        const int32_t timerID = timeout.timerIDCounter;

        const std::shared_ptr<TimeoutCallback> timeoutCallback = std::make_shared<TimeoutCallback>();
        timeoutCallback->jsContext = jsContext;
        timeoutCallback->callback = callback.release();
        timeoutCallback->timeout = &timeout;
        timeoutCallback->timerID = timerID;
        auto schedulerCallback = [timeoutCallback]() -> errors::Error {
            return timeoutCallback->call();
        };
        const std::expected<int64_t, errors::Error> callbackID = timeout.scheduler->registerCallback(std::move(schedulerCallback), now + delayDuration);
        if (!callbackID.has_value()) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: %s", callbackID.error().toString().c_str());
        }
        timeoutCallback->callbackID = callbackID.value();
        try {
            timeout.registry.insert_or_assign(timerID, callbackID.value());
        } catch (...) {
            const errors::Error error = timeout.scheduler->unregisterCallback(callbackID.value());
            if (error.errorCode) {
                std::terminate();
            }
            throw;
        }
        return JS_NewInt32(jsContext, timerID);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_ThrowInternalError(jsContext, "timeoutSet: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "timeoutSet: unexpected native failure");
    }
}

JSValue timeoutClear(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, Timeout& timeout) {
    try {
        if (timeout.scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: missing scheduler");
        }
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext, "timeoutClear: requires 1 argument (%d provided)", argc);
        }

        // Get timerID number.
        if (!JS_IsNumber(argv[0])) {
            return JS_ThrowTypeError(jsContext, "timeoutClear: arg0: timerID not a number");
        }
        double timerID = 0.0;
        const int status = JS_ToFloat64(jsContext, &timerID, argv[0]);
        if (status < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(timerID) || std::floor(timerID) != timerID || timerID < 0.0 || timerID > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            return JS_UNDEFINED;
        }
        const int32_t timerIDInt32 = static_cast<int32_t>(timerID);

        // If no such timerID exists, return.
        if (!timeout.registry.contains(timerIDInt32)) {
            return JS_UNDEFINED;
        }

        // Else unregister the timeout callback.
        const int64_t callbackID = timeout.registry.at(timerIDInt32);
        const errors::Error error = timeout.scheduler->unregisterCallback(callbackID);
        if (error.errorCode) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: %s", error.toString().c_str());
        }
        if (timeout.registry.contains(timerIDInt32) && timeout.registry.at(timerIDInt32) == callbackID) {
            timeout.registry.erase(timerIDInt32);
        }
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_ThrowInternalError(jsContext, "timeoutClear: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "timeoutClear: unexpected native failure");
    }
}

JSModuleDef* createTimeoutImplModule(JSContext* jsContext, Timeout& timeout) {
    try {
        if (timeout.scheduler == nullptr) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: missing scheduler");
            return nullptr;
        }

        // Get the jsRuntime and timeoutClassID.
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        JSClassID timeoutClassID = 0;
        {
            const std::lock_guard<std::mutex> lock(timeoutClassIDMutex);
            if (timeoutClassIDMap.contains(jsRuntime)) {
                timeoutClassID = timeoutClassIDMap.at(jsRuntime);
            } else {
                JS_NewClassID(jsRuntime, &timeoutClassID);
                timeoutClassIDMap.insert_or_assign(jsRuntime, timeoutClassID);
                const auto jsRuntimeFinalizer = [](JSRuntime* jsRuntime, void* /* argument */) {
                    const std::lock_guard<std::mutex> lock(timeoutClassIDMutex);
                    timeoutClassIDMap.erase(jsRuntime);
                };
                const int status = JS_AddRuntimeFinalizer(jsRuntime, jsRuntimeFinalizer, nullptr);
                if (status < 0) {
                    timeoutClassIDMap.erase(jsRuntime);
                    throw std::bad_alloc();
                }
            }
        }

        // Register the timeoutClassID if not already registered so that
        // JS_SetOpaque and JS_GetOpaque can work.
        if (!JS_IsRegisteredClass(jsRuntime, timeoutClassID)) {
            const JSClassDef jsClassDef = {
                .class_name = "Timeout",
                .finalizer = nullptr,
                .gc_mark = nullptr,
                .call = nullptr,
                .exotic = nullptr,
            };
            const int status = JS_NewClass(jsRuntime, timeoutClassID, &jsClassDef);
            if (status < 0) {
                JS_ThrowOutOfMemory(jsContext);
                return nullptr;
            }
        }

        // Create a modulePrivateValue JSValue and assign the Timeout* to it.
        ScopedJSValue modulePrivateValue(jsContext, JS_NewObjectProtoClass(jsContext, JS_NULL, timeoutClassID));
        if (JS_IsException(modulePrivateValue.value)) {
            return nullptr;
        }
        JS_SetOpaque(modulePrivateValue.value, &timeout);

        // Define the function entries to register.
        struct FunctionEntry {
            const char* name = nullptr;
            JSValue (*function)(JSContext*, JSValueConst, int, JSValueConst*, Timeout&) = nullptr;
            int argc = 0;
        };
        static const std::vector<FunctionEntry> functionEntries = {
            {
                .name = "timeoutSet",
                .function = timeoutSet,
                .argc = 2,
            },
            {
                .name = "timeoutClear",
                .function = timeoutClear,
                .argc = 1,
            },
        };

        // Create a new JS module.
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:timeout_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            // Extract the modulePrivateValue from the jsContext.
            const ScopedJSValue modulePrivateValue(jsContext, JS_GetModulePrivateValue(jsContext, jsModuleDef));
            if (JS_IsException(modulePrivateValue.value)) {
                return -1;
            }

            const auto jsCFunctionData = [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData) -> JSValue {
                // QuickJS retains the JS carrier, which borrows the native Timeout.
                Timeout* timeout = static_cast<Timeout*>(JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0])));
                if (timeout == nullptr) {
                    return JS_EXCEPTION;
                }
                if (magic < 0 || static_cast<std::size_t>(magic) >= functionEntries.size()) {
                    return JS_ThrowInternalError(jsContext, "Invalid timeout function index");
                }
                return functionEntries.at(static_cast<std::size_t>(magic)).function(jsContext, thisValue, argc, argv, *timeout);
            };
            std::vector<JSValue> functionData = {modulePrivateValue.value};
            for (std::size_t index = 0; index < functionEntries.size(); index++) {
                const FunctionEntry& functionEntry = functionEntries.at(index);
                const JSValue function = JS_NewCFunctionData(jsContext, jsCFunctionData, functionEntry.argc, static_cast<int>(index), static_cast<int>(functionData.size()), functionData.data());
                if (JS_IsException(function)) {
                    return -1;
                }
                const int status = JS_SetModuleExport(jsContext, jsModuleDef, functionEntry.name, function);
                if (status < 0) {
                    return -1;
                }
            }
            return 0;
        });
        if (jsModuleDef == nullptr) {
            return nullptr;
        }

        for (const FunctionEntry& functionEntry : functionEntries) {
            const int status = JS_AddModuleExport(jsContext, jsModuleDef, functionEntry.name);
            if (status < 0) {
                return nullptr;
            }
        }

        const int status = JS_SetModulePrivateValue(jsContext, jsModuleDef, modulePrivateValue.release());
        if (status < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(jsContext);
        return nullptr;
    } catch (const std::exception& exception) {
        if (!JS_HasException(jsContext)) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: %s", exception.what());
        }
        return nullptr;
    } catch (...) {
        if (!JS_HasException(jsContext)) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: unexpected native failure");
        }
        return nullptr;
    }
}

} // namespace amj::javascript
