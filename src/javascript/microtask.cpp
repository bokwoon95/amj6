#include "microtask.hpp"

#include <quickjs.h>

#include <array>

namespace amj::javascript {

JSValue queueMicrotask(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(jsContext, argv[0])) {
        return JS_ThrowTypeError(jsContext, "queueMicrotask requires a function");
    }
    const auto job = [](JSContext* jsContext, int /* argc */, JSValueConst* argv) -> JSValue {
        return JS_Call(jsContext, argv[0], JS_UNDEFINED, 0, nullptr);
    };
    const int status = JS_EnqueueJob(jsContext, job, 1, argv);
    if (status < 0) {
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

JSModuleDef* createMicrotaskModule(JSContext* jsContext) {
    static const std::array<JSCFunctionListEntry, 1> functionList = {{
        JS_CFUNC_DEF("queueMicrotask", 1, queueMicrotask),
    }};
    JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:microtask", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
        return JS_SetModuleExportList(jsContext, jsModuleDef, functionList.data(), static_cast<int>(functionList.size()));
    });
    if (jsModuleDef == nullptr) {
        return nullptr;
    }
    const int status = JS_AddModuleExportList(jsContext, jsModuleDef, functionList.data(), static_cast<int>(functionList.size()));
    if (status < 0) {
        return nullptr;
    }
    return jsModuleDef;
}

} // namespace amj::javascript
