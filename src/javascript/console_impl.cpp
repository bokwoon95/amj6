#include "console_impl.hpp"

#include "javascript.hpp"
#include "void_ptr/void_ptr.hpp"

#include <quickjs.h>

#include <exception>
#include <functional>
#include <new>
#include <optional>
#include <string>

namespace amj::javascript {
namespace void_ptr = amj::void_ptr;

JSValue consoleWrite(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) {
    try {
        if (argc < 2) {
            return JS_ThrowTypeError(jsContext, "consoleWrite requires a level and text");
        }
        if (!JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
            return JS_ThrowTypeError(jsContext, "consoleWrite level and text must be strings");
        }
        const std::optional<std::string> level = jsstring(jsContext, argv[0]);
        if (!level.has_value()) {
            return JS_EXCEPTION;
        }
        ConsoleData consoleData;
        if (level.value() == "log") {
            consoleData.level = ConsoleLevel::Log;
        } else if (level.value() == "debug") {
            consoleData.level = ConsoleLevel::Debug;
        } else if (level.value() == "info") {
            consoleData.level = ConsoleLevel::Info;
        } else if (level.value() == "warn") {
            consoleData.level = ConsoleLevel::Warn;
        } else if (level.value() == "error") {
            consoleData.level = ConsoleLevel::Error;
        } else {
            return JS_ThrowTypeError(jsContext, "consoleWrite level must be log, debug, info, warn, or error");
        }
        const std::optional<std::string> text = jsstring(jsContext, argv[1]);
        if (!text.has_value()) {
            return JS_EXCEPTION;
        }
        consoleData.text = text.value();
        const void* consoleDataPointer = JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0]));
        if (consoleDataPointer == nullptr) {
            return JS_EXCEPTION;
        }
        const std::function<void(ConsoleData)>* consoleSink = void_ptr::cast<std::function<void(ConsoleData)>*>(consoleDataPointer);
        (*consoleSink)(consoleData);
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "consoleWrite: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "consoleWrite: unexpected native failure");
    }
}

JSModuleDef* createConsoleImplModule(JSContext* jsContext, std::function<void(ConsoleData)>& consoleSink) {
    try {
        if (!consoleSink) {
            JS_ThrowTypeError(jsContext, "createConsoleImplModule requires a consoleSink callback");
            return nullptr;
        }

        // Each registration gets a runtime-local class ID; no global cache or
        // runtime-finalizer bookkeeping is needed for this native-only carrier.
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        JSClassID consoleClassID = 0;
        JS_NewClassID(jsRuntime, &consoleClassID);
        const JSClassDef jsClassDef = {
            .class_name = "ConsoleSink",
            .finalizer = [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                delete static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value, JS_GetClassID(value)));
            },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        int status = JS_NewClass(jsRuntime, consoleClassID, &jsClassDef);
        if (status < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        OwnedJSValue modulePrivateValue(jsContext, JS_NewObjectProtoClass(jsContext, JS_NULL, consoleClassID));
        if (JS_IsException(modulePrivateValue.get())) {
            return nullptr;
        }
        void_ptr::VoidPtr* voidPtr = new void_ptr::VoidPtr(&consoleSink);
        status = JS_SetOpaque(modulePrivateValue.get(), voidPtr);
        if (status < 0) {
            delete voidPtr;
            JS_ThrowInternalError(jsContext, "Could not attach console sink");
            return nullptr;
        }

        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:console_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            const OwnedJSValue modulePrivateValue(jsContext, JS_GetModulePrivateValue(jsContext, jsModuleDef));
            if (JS_IsException(modulePrivateValue.get())) {
                return -1;
            }
            JSValueConst functionData = modulePrivateValue.get();
            const JSValue function = JS_NewCFunctionData2(jsContext, consoleWrite, "consoleWrite", 2, 0, 1, &functionData);
            if (JS_IsException(function)) {
                return -1;
            }
            // SetModuleExport consumes function on both success and failure.
            return JS_SetModuleExport(jsContext, jsModuleDef, "consoleWrite", function);
        });
        if (jsModuleDef == nullptr) {
            return nullptr;
        }
        status = JS_AddModuleExport(jsContext, jsModuleDef, "consoleWrite");
        if (status < 0) {
            return nullptr;
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
        JS_ThrowInternalError(jsContext, "createConsoleImplModule: %s", exception.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(jsContext, "createConsoleImplModule: unexpected native failure");
        return nullptr;
    }
}

} // namespace amj::javascript
