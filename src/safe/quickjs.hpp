#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <expected>
#include <optional>
#include <source_location>
#include <system_error>

namespace amj::safe {

class QuickJSRuntime {
public:
    [[nodiscard]] static std::expected<QuickJSRuntime, std::error_code> create() noexcept;

    ~QuickJSRuntime() noexcept;
    QuickJSRuntime(const QuickJSRuntime&) = delete;
    QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;
    QuickJSRuntime(QuickJSRuntime&& other) noexcept;
    QuickJSRuntime& operator=(QuickJSRuntime&&) = delete;

    [[nodiscard]] JSRuntime*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsRuntime == nullptr) {
            fail("access through a closed or moved QuickJS runtime", sourceLocation);
        }
        return jsRuntime;
    }

    [[nodiscard]] Borrowed<QuickJSRuntime>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsRuntime == nullptr) {
            fail("borrowing a closed or moved QuickJS runtime", sourceLocation);
        }
        return Borrowed<QuickJSRuntime>(sourceLocation, head, jsRuntime);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    QuickJSRuntime() noexcept = default;

    JSRuntime* jsRuntime = nullptr;
    Borrower head = {};
};

class QuickJSContext {
public:
    [[nodiscard]] static std::expected<QuickJSContext, std::error_code>
    create(Borrowed<QuickJSRuntime> quickjsRuntime) noexcept;

    ~QuickJSContext() noexcept;
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;
    QuickJSContext(QuickJSContext&& other) noexcept;
    QuickJSContext& operator=(QuickJSContext&&) = delete;

    [[nodiscard]] JSContext*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (jsContext == nullptr) {
            fail("access through a closed or moved QuickJS context", sourceLocation);
        }
        return jsContext;
    }

    [[nodiscard]] Borrowed<QuickJSContext>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (jsContext == nullptr) {
            fail("borrowing a closed or moved QuickJS context", sourceLocation);
        }
        return Borrowed<QuickJSContext>(sourceLocation, head, jsContext);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    QuickJSContext() noexcept = default;

    std::optional<Borrowed<QuickJSRuntime>> quickjsRuntime = std::nullopt;
    JSContext* jsContext = nullptr;
    Borrower head = {};
};

// Adopts one owned JSValue reference. Call JS_DupValue explicitly when starting
// from a borrowed value. The context borrow remains registered through cleanup.
class QuickJSValue {
public:
    QuickJSValue(Borrowed<QuickJSContext> context, JSValue value) noexcept;
    ~QuickJSValue() noexcept;
    QuickJSValue(const QuickJSValue&) = delete;
    QuickJSValue& operator=(const QuickJSValue&) = delete;
    QuickJSValue(QuickJSValue&& other) noexcept;
    QuickJSValue& operator=(QuickJSValue&&) = delete;

    [[nodiscard]] JSValue
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (!live) {
            fail("access through a closed or moved QuickJS value", sourceLocation);
        }
        return value;
    }

    [[nodiscard]] Borrowed<QuickJSValue>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (!live) {
            fail("borrowing a closed or moved QuickJS value", sourceLocation);
        }
        return Borrowed<QuickJSValue>(sourceLocation, head, value);
    }
    [[nodiscard]] JSValue

    release(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    std::optional<Borrowed<QuickJSContext>> context = std::nullopt;
    JSValue value = JS_UNDEFINED;
    bool live = false;
    Borrower head = {};
};

struct QuickJSPromise {
    std::optional<QuickJSValue> promise = std::nullopt;
    std::optional<QuickJSValue> resolveFunction = std::nullopt;
    std::optional<QuickJSValue> rejectFunction = std::nullopt;
};

} // namespace amj::safe
