#pragma once

#include <quickjs.h>

#include <stdexcept>
#include <utility>

namespace amj::owned {

// Owns one QuickJS reference. The borrowed context must outlive this wrapper.
// All operations, including destruction, require serialized runtime access.
class JSValue {
  public:
    JSValue() = default;

    JSValue(JSContext* jsContext, ::JSValue jsValue) {
        if (jsContext == nullptr) {
            throw std::invalid_argument("JSValue requires a context");
        }
        this->jsContext = jsContext;
        this->jsValue = jsValue;
    }

    ~JSValue() {
        if (jsContext != nullptr) {
            JS_FreeValue(jsContext, jsValue);
        }
    }

    JSValue(const JSValue&) = delete;
    JSValue& operator=(const JSValue&) = delete;

    JSValue(JSValue&& jsValue) noexcept {
        this->jsContext = std::exchange(jsValue.jsContext, nullptr);
        this->jsValue = jsValue.release();
    }

    JSValue& operator=(JSValue&& jsValue) noexcept {
        if (this != &jsValue) {
            if (this->jsContext != nullptr) {
                JS_FreeValue(this->jsContext, this->jsValue);
            }
            this->jsContext = std::exchange(jsValue.jsContext, nullptr);
            this->jsValue = jsValue.release();
        }
        return *this;
    }

    ::JSValue& get() noexcept {
        return jsValue;
    }

    const ::JSValue& get() const noexcept {
        return jsValue;
    }

    // Adopts a reference belonging to the existing context. Does not duplicate it.
    void reset(::JSValue jsValue) {
        if (jsContext == nullptr) {
            throw std::logic_error("Cannot reset a JSValue without a context");
        }
        JS_FreeValue(jsContext, std::exchange(this->jsValue, jsValue));
    }

    ::JSValue release() noexcept {
        return std::exchange(jsValue, JS_UNDEFINED);
    }

  private:
    JSContext* jsContext = nullptr; // Borrowed.
    ::JSValue jsValue = JS_UNDEFINED;
};

// Owns the promise reference and both settlement-function references.
struct JSPromise {
    JSValue promise = {};
    JSValue resolveFunction = {};
    JSValue rejectFunction = {};
};

} // namespace amj::owned
