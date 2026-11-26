#pragma once

#include "safe.hpp"

#include <quickjs.h>

#include <optional>
#include <source_location>
#include <system_error>

namespace amj::safe {

class QuickJSRuntime {
public:
    QuickJSRuntime();

    ~QuickJSRuntime() noexcept;
    QuickJSRuntime(const QuickJSRuntime&) = delete;
    QuickJSRuntime& operator=(const QuickJSRuntime&) = delete;
    QuickJSRuntime(QuickJSRuntime&& other) noexcept;
    QuickJSRuntime& operator=(QuickJSRuntime&&) = delete;

    [[nodiscard]] JSRuntime*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSRuntime>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        return Borrowed<QuickJSRuntime>::makeBorrowed(this->get(sourceLocation), this->head,
            sourceLocation);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    JSRuntime* jsRuntime = nullptr;
    Borrower head = {};
};

class QuickJSContext {
public:
    explicit QuickJSContext(Borrowed<QuickJSRuntime> quickjsRuntime);

    ~QuickJSContext() noexcept;
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;
    QuickJSContext(QuickJSContext&& other) noexcept;
    QuickJSContext& operator=(QuickJSContext&&) = delete;

    [[nodiscard]] JSContext*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSContext>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        return Borrowed<QuickJSContext>::makeBorrowed(this->get(sourceLocation), this->head,
            sourceLocation);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
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
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<QuickJSValue>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        return Borrowed<QuickJSValue>::makeBorrowed(this->get(sourceLocation), this->head,
            sourceLocation);
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
