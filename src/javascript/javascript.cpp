#include "javascript.hpp"

#include "owned/quickjs.hpp"

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
namespace owned = amj::owned;
namespace {

JSValue jsonValueToJSValue(JSContext* jsContext, const JSONValue& jsonValue, int depth) {
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
        owned::JSValue result(jsContext, JS_NewArray(jsContext));
        if (JS_IsException(result.get())) {
            return JS_EXCEPTION;
        }
        for (std::size_t index = 0; index < array->size(); index++) {
            const JSValue element = jsonValueToJSValue(jsContext, array->at(index), depth + 1);
            if (JS_IsException(element)) {
                return JS_EXCEPTION;
            }
            if (JS_DefinePropertyValueUint32(jsContext, result.get(), static_cast<uint32_t>(index),
                    element, JS_PROP_C_W_E) < 0) {
                return JS_EXCEPTION;
            }
        }
        return result.release();
    }
    const std::map<std::string, JSONValue>* object =
        std::get_if<std::map<std::string, JSONValue>>(&jsonValue.value);
    if (object != nullptr) {
        owned::JSValue result(jsContext, JS_NewObjectProto(jsContext, JS_NULL));
        if (JS_IsException(result.get())) {
            return JS_EXCEPTION;
        }
        for (const std::pair<const std::string, JSONValue>& field : *object) {
            owned::JSValue element(jsContext,
                jsonValueToJSValue(jsContext, field.second, depth + 1));
            if (JS_IsException(element.get())) {
                return JS_EXCEPTION;
            }
            const JSAtom key = JS_NewAtomLen(jsContext, field.first.data(), field.first.size());
            if (key == JS_ATOM_NULL) {
                return JS_EXCEPTION;
            }
            const int status = JS_DefinePropertyValue(jsContext, result.get(), key,
                element.release(), JS_PROP_C_W_E);
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

owned::JSValue jsNewError(JSContext* jsContext, std::string name, std::string message) {
    // Create error.
    owned::JSValue error(jsContext, JS_NewError(jsContext));
    if (JS_IsException(error.get())) {
        return owned::JSValue(jsContext, JS_GetException(jsContext));
    }
    // Set name property.
    owned::JSValue nameString(jsContext, JS_NewStringLen(jsContext, name.data(), name.size()));
    if (JS_IsException(nameString.get())) {
        return owned::JSValue(jsContext, JS_GetException(jsContext));
    }
    int status = JS_DefinePropertyValueStr(jsContext, error.get(), "name", nameString.release(),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    if (status < 0) {
        return owned::JSValue(jsContext, JS_GetException(jsContext));
    }
    // Set message property.
    owned::JSValue messageString(jsContext,
        JS_NewStringLen(jsContext, message.data(), message.size()));
    if (JS_IsException(messageString.get())) {
        return owned::JSValue(jsContext, JS_GetException(jsContext));
    }
    status = JS_DefinePropertyValueStr(jsContext, error.get(), "message", messageString.release(),
        JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    if (status < 0) {
        return owned::JSValue(jsContext, JS_GetException(jsContext));
    }
    return error;
}

JSValue JSONValue::toJSValue(JSContext* jsContext) const try {
    return jsonValueToJSValue(jsContext, *this, 0);
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

std::optional<std::string> jsToString(JSContext* jsContext, JSValueConst jsValueConst) {
    if (JS_IsException(jsValueConst)) {
        return std::nullopt;
    }
    std::size_t len = 0;
    const char* cString = JS_ToCStringLen(jsContext, &len, jsValueConst);
    if (cString == nullptr) {
        return std::nullopt;
    }
    const auto freeCString = [jsContext](const char* cString) {
        JS_FreeCString(jsContext, cString);
    };
    const std::unique_ptr<const char, decltype(freeCString)> ownedCString(cString, freeCString);
    return std::string(cString, len);
}

std::optional<uint32_t> jsToUint32(JSContext* jsContext, JSValueConst jsValueConst) {
    if (JS_IsException(jsValueConst)) {
        return std::nullopt;
    }
    uint32_t num = 0;
    const int status = JS_ToUint32(jsContext, &num, jsValueConst);
    if (status < 0) {
        return std::nullopt;
    }
    return num;
}

bool setstringfield(JSContext* jsContext, JSValueConst object, const char* key,
    std::string_view value) {
    const JSValue text = JS_NewStringLen(jsContext, value.data(), value.size());
    if (JS_IsException(text)) {
        return false;
    }
    return JS_DefinePropertyValueStr(jsContext, object, key, text, JS_PROP_C_W_E) >= 0;
}

bool equalFoldASCII(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); index++) {
        char leftChar = left.at(index);
        char rightChar = right.at(index);
        if (leftChar >= 'a' && leftChar <= 'z') {
            leftChar = static_cast<char>(leftChar - 'a' + 'A');
        }
        if (rightChar >= 'a' && rightChar <= 'z') {
            rightChar = static_cast<char>(rightChar - 'a' + 'A');
        }
        if (leftChar != rightChar) {
            return false;
        }
    }
    return true;
}

} // namespace amj::javascript
