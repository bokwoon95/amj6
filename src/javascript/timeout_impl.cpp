#include "timeout_impl.hpp"

#include "javascript.hpp"

#include <quickjs.h>
#include <uv.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace this_thread = std::this_thread;

namespace {

std::mutex timeoutClassIDMutex = {};
std::map<JSRuntime*, JSClassID> timeoutClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

struct TimerHandleData {
    Timeout* timeout = nullptr;
    int32_t timerID = 0;
};

} // namespace

Timeout::Timeout(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> callbackDidRun) {
    if (jsContext == nullptr || uvLoop == nullptr || callbackDidRun == nullptr) {
        throw std::invalid_argument("Timeout.constructor: missing context, loop, or completion hook");
    }
    this->jsRuntime = JS_GetRuntime(jsContext);
    this->jsContext = jsContext;
    this->uvLoop = uvLoop;
    this->callbackDidRun = std::move(callbackDidRun);
    this->ownerThreadID = this_thread::get_id();
}

Timeout::~Timeout() {
    shutdown();
}

void Timeout::shutdown() {
    if (this_thread::get_id() != ownerThreadID) {
        std::terminate();
    }
    if (shuttingDown) {
        return; // Safe even after the context/runtime have been destroyed.
    }
    shuttingDown = true;
    for (const std::pair<const int32_t, Timer>& entry : timerMap) {
        uv_close(reinterpret_cast<uv_handle_t*>(entry.second.uvTimerHandle), [](uv_handle_t* uvHandle) {
            const uv_timer_t* uvTimerHandle = reinterpret_cast<uv_timer_t*>(uvHandle);
            const TimerHandleData* timerHandleData = static_cast<TimerHandleData*>(uvTimerHandle->data);
            delete timerHandleData;
            delete uvTimerHandle;
        });
        JS_FreeValueRT(jsRuntime, entry.second.callback);
    }
    timerMap.clear();
}

JSValue timeoutSet(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, Timeout& timeout) {
    try {
        if (this_thread::get_id() != timeout.ownerThreadID) {
            std::terminate(); // Even creating a JS exception requires the owner thread.
        }
        if (jsContext != timeout.jsContext) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: wrong context");
        }
        if (timeout.shuttingDown) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: owner is shut down");
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
        int status = JS_ToFloat64(jsContext, &delayMs, argv[1]);
        if (status < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(delayMs) || std::floor(delayMs) != delayMs || delayMs < 0.0 || delayMs > static_cast<double>((std::numeric_limits<int32_t>::max)())) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: arg1: delay must be a positive number within 32-bit signed integer range");
        }

        // Allocate the next unused positive timer ID, wrapping at INT32_MAX.
        while (true) {
            if (timeout.timerMap.size() >= static_cast<std::size_t>((std::numeric_limits<int32_t>::max)())) {
                return JS_ThrowRangeError(jsContext, "timeoutSet: timerIDs exhausted");
            }
            if (timeout.timerIDCounter == (std::numeric_limits<int32_t>::max)()) {
                timeout.timerIDCounter = 1;
            } else {
                timeout.timerIDCounter++;
            }
            if (timeout.timerMap.contains(timeout.timerIDCounter)) {
                continue;
            }
            break;
        }
        const int32_t timerID = timeout.timerIDCounter;

        // Finish allocations before initializing the libuv handle.
        std::unique_ptr<uv_timer_t> uvTimerHandle = std::make_unique<uv_timer_t>();
        std::unique_ptr<TimerHandleData> timerHandleData = std::make_unique<TimerHandleData>();
        timerHandleData->timeout = &timeout;
        timerHandleData->timerID = timerID;
        timeout.timerMap.insert_or_assign(timerID, Timeout::Timer{});
        status = uv_timer_init(timeout.uvLoop, uvTimerHandle.get());
        if (status != 0) {
            timeout.timerMap.erase(timerID);
            return JS_ThrowInternalError(jsContext, "timeoutSet: uv_timer_init: %s", uv_strerror(status));
        }
        uvTimerHandle->data = timerHandleData.release();
        timeout.timerMap.at(timerID).callback = callback.release();
        // Libuv caches loop time. Refresh it so delays start at registration,
        // even if the host has been executing JS for a long time this turn.
        uv_update_time(timeout.uvLoop);
        const auto uvTimerCallback = [](uv_timer_t* uvTimerHandle) noexcept {
            try {
                const TimerHandleData* timerHandleData = static_cast<TimerHandleData*>(uvTimerHandle->data);
                const std::map<int32_t, Timeout::Timer>::node_type mapNode = timerHandleData->timeout->timerMap.extract(timerHandleData->timerID);
                if (mapNode.empty()) {
                    std::terminate(); // An active timer must have a registration.
                }
                const Timeout::Timer& timer = mapNode.mapped();

                // Close uvTimerHandle and delete it and its HandleData after closure.
                uv_close(reinterpret_cast<uv_handle_t*>(uvTimerHandle), [](uv_handle_t* uvHandle) {
                    const uv_timer_t* uvTimerHandle = reinterpret_cast<uv_timer_t*>(uvHandle);
                    const TimerHandleData* timerHandleData = static_cast<TimerHandleData*>(uvTimerHandle->data);
                    delete timerHandleData;
                    delete uvTimerHandle;
                });

                // Call the callback and discard the result.
                const JSValue result = JS_Call(timerHandleData->timeout->jsContext, timer.callback, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(timerHandleData->timeout->jsContext, result);
                JS_FreeValue(timerHandleData->timeout->jsContext, timer.callback);

                // Let the host report any JS exceptions and execute pending Promise jobs.
                timerHandleData->timeout->callbackDidRun(timerHandleData->timeout->jsContext);
            } catch (...) {
                std::terminate();
            }
        };
        status = uv_timer_start(uvTimerHandle.get(), uvTimerCallback, static_cast<uint64_t>(delayMs), 0);
        if (status != 0) {
            const std::map<int32_t, Timeout::Timer>::node_type mapNode = timeout.timerMap.extract(timerID);
            if (mapNode.empty()) {
                std::terminate(); // The initialized timer must have a registration.
            }
            uv_close(reinterpret_cast<uv_handle_t*>(uvTimerHandle.release()), [](uv_handle_t* uvHandle) {
                const uv_timer_t* uvTimerHandle = reinterpret_cast<uv_timer_t*>(uvHandle);
                const TimerHandleData* timerHandleData = static_cast<TimerHandleData*>(uvTimerHandle->data);
                delete timerHandleData;
                delete uvTimerHandle;
            });
            JS_FreeValue(jsContext, mapNode.mapped().callback);
            return JS_ThrowInternalError(jsContext, "timeoutSet: uv_timer_start: %s", uv_strerror(status));
        }
        timeout.timerMap.at(timerID).uvTimerHandle = uvTimerHandle.release();
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
        if (this_thread::get_id() != timeout.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != timeout.jsContext) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: wrong context");
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
        if (!std::isfinite(timerID) || std::floor(timerID) != timerID || timerID < 0.0 || timerID > static_cast<double>((std::numeric_limits<int32_t>::max)())) {
            return JS_UNDEFINED;
        }
        const int32_t timerIDInt32 = static_cast<int32_t>(timerID);

        const std::map<int32_t, Timeout::Timer>::node_type mapNode = timeout.timerMap.extract(timerIDInt32);
        if (mapNode.empty()) {
            return JS_UNDEFINED;
        }
        const Timeout::Timer& timer = mapNode.mapped();

        uv_close(reinterpret_cast<uv_handle_t*>(timer.uvTimerHandle), [](uv_handle_t* uvHandle) {
            const uv_timer_t* uvTimerHandle = reinterpret_cast<uv_timer_t*>(uvHandle);
            const TimerHandleData* timerHandleData = static_cast<TimerHandleData*>(uvTimerHandle->data);
            delete timerHandleData;
            delete uvTimerHandle;
        });
        JS_FreeValue(jsContext, timer.callback);
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
        if (this_thread::get_id() != timeout.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != timeout.jsContext || timeout.shuttingDown) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: wrong context or owner is shut down");
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
                .gc_mark = nullptr, // Callback references belong to the host-owned Timeout.
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
        if (JS_IsException(modulePrivateValue.get())) {
            return nullptr;
        }
        int status = JS_SetOpaque(modulePrivateValue.get(), &timeout);
        if (status < 0) {
            JS_ThrowInternalError(jsContext, "Could not attach Timeout");
            return nullptr;
        }

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
            if (JS_IsException(modulePrivateValue.get())) {
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
            std::vector<JSValue> functionData = {modulePrivateValue.get()};
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

        status = JS_SetModulePrivateValue(jsContext, jsModuleDef, modulePrivateValue.release());
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
