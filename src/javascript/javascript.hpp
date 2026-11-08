#pragma once

#include <quickjs.h>
#include <uv.h>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

// Owns an initialized libuv handle and its heap-allocated data. The handle must
// have null data and must not already be closing when adopted. Do not separately
// close the borrowed handle or replace its data. Use on the loop's owner thread.
// close() and destruction schedule cleanup; keep driving the loop until its close
// callbacks finish. U's destructor must not throw or depend on expired objects.
template <typename T, typename U>
class OwnedUvHandle {
    static_assert(
        std::is_same_v<T, uv_timer_t> || std::is_same_v<T, uv_async_t> || std::is_same_v<T, uv_poll_t>,
        "OwnedUvHandle requires a supported libuv handle type"
    );
    static_assert(std::is_object_v<U> && !std::is_array_v<U>, "OwnedUvHandle data must be a single object");

  public:
    OwnedUvHandle() = default;
    // Validation failures leave both arguments owned by the caller.
    OwnedUvHandle(std::unique_ptr<T>&& handle, std::unique_ptr<U>&& data) {
        if (handle == nullptr || data == nullptr) {
            throw std::invalid_argument("OwnedUvHandle requires a handle and data");
        }
        if (handle->data != nullptr) {
            throw std::invalid_argument("OwnedUvHandle requires null handle data");
        }
        if (uv_is_closing(reinterpret_cast<uv_handle_t*>(handle.get())) != 0) {
            throw std::invalid_argument("OwnedUvHandle cannot adopt a closing handle");
        }
        handle->data = data.release();
        this->uvHandle = handle.release();
    }
    ~OwnedUvHandle() {
        close();
    }
    OwnedUvHandle(const OwnedUvHandle&) = delete;
    OwnedUvHandle& operator=(const OwnedUvHandle&) = delete;
    OwnedUvHandle(OwnedUvHandle&& ownedUvHandle) noexcept {
        this->uvHandle = std::exchange(ownedUvHandle.uvHandle, nullptr);
    }
    OwnedUvHandle& operator=(OwnedUvHandle&& ownedUvHandle) noexcept {
        if (this != &ownedUvHandle) {
            close();
            this->uvHandle = std::exchange(ownedUvHandle.uvHandle, nullptr);
        }
        return *this;
    }
    T* get() const {
        return uvHandle;
    }
    U* getData() const {
        if (uvHandle == nullptr) {
            return nullptr;
        }
        return static_cast<U*>(uvHandle->data);
    }
    void close() noexcept {
        T* handle = std::exchange(this->uvHandle, nullptr);
        if (handle == nullptr) {
            return;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* uvHandle) noexcept {
            const T* handle = reinterpret_cast<T*>(uvHandle);
            const U* data = static_cast<U*>(handle->data);
            delete data;
            delete handle;
        });
    }

  private:
    T* uvHandle = nullptr;
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
