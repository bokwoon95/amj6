#pragma once

#include <quickjs.h>

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amj::javascript {

// Owns one QuickJS reference and releases it on destruction unless release()
// is called. Noncopyable. A default-constructed wrapper is empty and has no
// context. Otherwise, the context must outlive this wrapper. Serialize QuickJS
// operations across the entire runtime.
class OwnedJSValue {
  public:
    OwnedJSValue() = default;
    OwnedJSValue(JSContext* jsContext, JSValue jsValue) {
        this->jsContext = jsContext;
        this->jsValue = jsValue;
    }
    ~OwnedJSValue() {
        if (jsContext != nullptr) {
            JS_FreeValue(jsContext, jsValue);
        }
    }
    OwnedJSValue(const OwnedJSValue&) = delete;
    OwnedJSValue& operator=(const OwnedJSValue&) = delete;
    OwnedJSValue(OwnedJSValue&& ownedJSValue) noexcept {
        this->jsContext = ownedJSValue.jsContext;
        this->jsValue = ownedJSValue.release();
    }
    OwnedJSValue& operator=(OwnedJSValue&& ownedJSValue) noexcept {
        if (this != &ownedJSValue) {
            const OwnedJSValue previous(this->jsContext, this->release());
            this->jsContext = ownedJSValue.jsContext;
            this->jsValue = ownedJSValue.release();
        }
        return *this;
    }
    JSValue& get() {
        return jsValue;
    }
    const JSValue& get() const {
        return jsValue;
    }
    void reset(JSValue jsValue) {
        if (jsContext == nullptr) {
            throw std::logic_error("Cannot reset an OwnedJSValue without a context; assign a context-bound wrapper instead");
        }
        JS_FreeValue(jsContext, std::exchange(this->jsValue, jsValue));
    }
    JSValue release() {
        return std::exchange(jsValue, JS_UNDEFINED);
    }

  private:
    JSContext* jsContext = nullptr;
    JSValue jsValue = JS_UNDEFINED;
};

OwnedJSValue jsNewError(JSContext* jsContext, std::string name, std::string message);

// Owns a promise and its settlement functions. Duplicate the promise when passing
// ownership to JavaScript. Callers release its settlement functions when used.
struct OwnedJSPromise {
    OwnedJSValue promise = {};
    OwnedJSValue resolveFunction = {};
    OwnedJSValue rejectFunction = {};
};

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

// Defines an own, writable/enumerable/configurable string property.
// Returns false with a pending JS exception on failure.
bool setstringfield(JSContext* jsContext, JSValueConst object, const char* key, std::string_view value);

} // namespace amj::javascript
