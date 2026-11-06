#include "javascript.hpp"

#include <quickjs.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amj::javascript {
namespace {

JSValue convertJSON(JSContext* jsContext, const JSONValue& jsonValue, int depth) {
    if (depth > 128) {
        return JS_ThrowRangeError(jsContext, "JSON nesting exceeds 128 levels");
    }
    if (std::holds_alternative<std::nullptr_t>(jsonValue.value)) {
        return JS_NULL;
    }
    const bool* boolean = std::get_if<bool>(&jsonValue.value);
    if (boolean != nullptr) {
        return JS_NewBool(jsContext, *boolean);
    }
    const double* number = std::get_if<double>(&jsonValue.value);
    if (number != nullptr) {
        if (!std::isfinite(*number)) {
            return JS_ThrowRangeError(jsContext, "JSON numbers must be finite");
        }
        return JS_NewFloat64(jsContext, *number);
    }
    const std::string* string = std::get_if<std::string>(&jsonValue.value);
    if (string != nullptr) {
        return JS_NewStringLen(jsContext, string->data(), string->size());
    }
    const std::vector<JSONValue>* array = std::get_if<std::vector<JSONValue>>(&jsonValue.value);
    if (array != nullptr) {
        if (array->size() > std::numeric_limits<uint32_t>::max()) {
            return JS_ThrowRangeError(jsContext, "JSON array exceeds the JS array length limit");
        }
        ScopedJSValue result(jsContext, JS_NewArray(jsContext));
        if (JS_IsException(result.value)) {
            return JS_EXCEPTION;
        }
        for (std::size_t index = 0; index < array->size(); index++) {
            const JSValue element = convertJSON(jsContext, array->at(index), depth + 1);
            if (JS_IsException(element)) {
                return JS_EXCEPTION;
            }
            if (JS_DefinePropertyValueUint32(jsContext, result.value, static_cast<uint32_t>(index), element, JS_PROP_C_W_E) < 0) {
                return JS_EXCEPTION;
            }
        }
        return result.release();
    }
    const std::map<std::string, JSONValue>* object = std::get_if<std::map<std::string, JSONValue>>(&jsonValue.value);
    if (object != nullptr) {
        ScopedJSValue result(jsContext, JS_NewObjectProto(jsContext, JS_NULL));
        if (JS_IsException(result.value)) {
            return JS_EXCEPTION;
        }
        for (const std::pair<const std::string, JSONValue>& field : *object) {
            ScopedJSValue element(jsContext, convertJSON(jsContext, field.second, depth + 1));
            if (JS_IsException(element.value)) {
                return JS_EXCEPTION;
            }
            const JSAtom key = JS_NewAtomLen(jsContext, field.first.data(), field.first.size());
            if (key == JS_ATOM_NULL) {
                return JS_EXCEPTION;
            }
            const int status = JS_DefinePropertyValue(jsContext, result.value, key, element.release(), JS_PROP_C_W_E);
            JS_FreeAtom(jsContext, key);
            if (status < 0) {
                return JS_EXCEPTION;
            }
        }
        return result.release();
    }
    return JS_ThrowInternalError(jsContext, "JSON value has no active alternative");
}

} // namespace

JSValue JSONValue::toJSValue(JSContext* jsContext) const try {
    return convertJSON(jsContext, *this, 0);
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected JSON conversion failure");
}

std::optional<std::string> jsstring(JSContext* jsContext, JSValueConst value) {
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(jsContext, &length, value);
    if (text == nullptr) {
        return std::nullopt;
    }
    const auto release = [jsContext](const char* data) {
        JS_FreeCString(jsContext, data);
    };
    const std::unique_ptr<const char, decltype(release)> ownedText(text, release);
    return std::string(text, length);
}

bool setstringfield(const ScopedJSValue& obj, const char* key, std::string_view value) {
    const JSValue text = JS_NewStringLen(obj.jsContext, value.data(), value.size());
    if (JS_IsException(text)) {
        return false;
    }
    return JS_DefinePropertyValueStr(obj.jsContext, obj.value, key, text, JS_PROP_C_W_E) >= 0;
}

} // namespace amj::javascript
