#pragma once

#include "owned/quickjs.hpp"

#include <cstdint>
#include <quickjs.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amj::javascript {
namespace owned = amj::owned;

owned::JSValue jsNewError(JSContext* jsContext, std::string name, std::string message);

// An in-memory JSON tree, not serialized JSON text. Numbers must be finite.
// MSVC map moves can allocate; implicit moves may throw and callers handle allocation failures.
struct JSONValue { // NOLINT(bugprone-exception-escape)
    std::variant<std::nullptr_t, bool, double, std::string, std::vector<JSONValue>, std::map<std::string, JSONValue>> value = nullptr;

    bool operator==(const JSONValue& jsonValue) const {
        return this->value == jsonValue.value;
    }

    JSONValue() = default;

    JSONValue(std::nullptr_t) {
        this->value = nullptr;
    }

    JSONValue(bool boolean) {
        this->value = boolean;
    }

    JSONValue(double number) {
        this->value = number;
    }

    JSONValue(std::string string) {
        this->value = std::move(string);
    }

    JSONValue(const char* string) {
        if (string == nullptr) {
            this->value = nullptr;
        } else {
            this->value = std::string(string);
        }
    }

    JSONValue(std::vector<JSONValue> array) {
        this->value = std::move(array);
    }

    JSONValue(std::map<std::string, JSONValue> object) {
        this->value = std::move(object);
    }

    // Creates an owned JS value. Objects have a null prototype. Returns JS_EXCEPTION
    // with a pending exception on failure, including nesting deeper than 128 levels.
    JSValue toJSValue(JSContext* jsContext) const;
};

// Converts to UTF-8 without type validation or size limits. Returns nullopt on
// failure with a pending JS exception. Check JS_IsString() first if coercion is unwanted.
std::optional<std::string> jsstring(JSContext* jsContext, JSValueConst value);

// Returns nullopt on failure with a pending JS exception.
std::optional<std::string> jsToString(JSContext* jsContext, JSValueConst jsValueConst);

// Returns nullopt on failure with a pending JS exception.
std::optional<uint32_t> jsToUint32(JSContext* jsContext, JSValueConst jsValueConst);

// Defines an own, writable/enumerable/configurable string property.
// Returns false with a pending JS exception on failure.
bool setstringfield(JSContext* jsContext, JSValueConst object, const char* key, std::string_view value);

bool equalFoldASCII(std::string_view left, std::string_view right);

} // namespace amj::javascript
