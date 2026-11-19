#pragma once

#include "errors/errors.hpp"

#include <quickjs.h>

#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace amj::owned {
namespace errors = amj::errors;

// Owns one QuickJS reference. The borrowed context must outlive this wrapper.
// All operations, including destruction, require serialized runtime access.
class JSValue {
  public:
    JSValue() = default;

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

    // Frees the reference and clears the borrowed context.
    void reset() noexcept {
        if (jsContext != nullptr) {
            JS_FreeValue(jsContext, jsValue);
        }
        jsContext = nullptr;
        jsValue = JS_UNDEFINED;
    }

    ::JSValue release() noexcept {
        return std::exchange(jsValue, JS_UNDEFINED);
    }

  private:
    friend std::expected<JSValue, errors::Error> createJSValue(JSContext* jsContext,
        ::JSValue jsValue);

    JSContext* jsContext = nullptr; // Borrowed.
    ::JSValue jsValue = JS_UNDEFINED;
};

// Takes ownership on success. A null context is rejected without consuming
// jsValue. Internal callers with a known live context may use .value().
inline std::expected<JSValue, errors::Error> createJSValue(JSContext* jsContext,
    ::JSValue jsValue) {
    if (jsContext == nullptr) {
        return std::unexpected(errors::Error{
            .errorCode = std::make_error_code(std::errc::invalid_argument),
            .errorMessage = std::make_shared<const std::string>("JSValue requires a context"),
        });
    }
    JSValue ownedJSValue;
    ownedJSValue.jsContext = jsContext;
    ownedJSValue.jsValue = jsValue;
    return ownedJSValue;
}

// Owns the promise reference and both settlement-function references.
struct JSPromise {
    JSValue promise = {};
    JSValue resolveFunction = {};
    JSValue rejectFunction = {};
};

} // namespace amj::owned
