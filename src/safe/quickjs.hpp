#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <optional>
#include <system_error>

namespace amj::safe {

class OwnedJSValue {
public:
    OwnedJSValue(const Borrowed<JSContext*>& jsContext, JSValue jsValue);
    ~OwnedJSValue() noexcept;
    OwnedJSValue(const OwnedJSValue&) = delete;
    OwnedJSValue& operator=(const OwnedJSValue&) = delete;
    OwnedJSValue(OwnedJSValue&&) noexcept = default;
    OwnedJSValue& operator=(OwnedJSValue&&) = delete;

    [[nodiscard]] JSValue get() const;
    [[nodiscard]] JSValue release();
    [[nodiscard]] std::error_code close() noexcept;

private:
    std::optional<Borrowed<JSContext*>> jsContext = std::nullopt;
    JSValue jsValue = JS_UNDEFINED;
    std::shared_ptr<Lifetime> lifetime = nullptr;
};

struct OwnedJSPromise {
    std::optional<OwnedJSValue> promise = std::nullopt;
    std::optional<OwnedJSValue> resolveFunction = std::nullopt;
    std::optional<OwnedJSValue> rejectFunction = std::nullopt;
};

} // namespace amj::safe
