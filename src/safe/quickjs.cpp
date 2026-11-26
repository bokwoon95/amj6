#include "quickjs.hpp"

#include "safe.hpp"

#include <quickjs.h>

#include <expected>
#include <source_location>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {

std::expected<QuickJSRuntime, std::error_code> QuickJSRuntime::create() noexcept {
    QuickJSRuntime quickJSRuntime;
    quickJSRuntime.jsRuntime = JS_NewRuntime();
    if (quickJSRuntime.jsRuntime == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return quickJSRuntime;
}

QuickJSRuntime::~QuickJSRuntime() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        fail("QuickJS runtime cleanup failed", std::source_location::current());
    }
}

QuickJSRuntime::QuickJSRuntime(QuickJSRuntime&& other) noexcept : head(std::move(other.head)) {
    jsRuntime = std::exchange(other.jsRuntime, nullptr);
}

std::error_code QuickJSRuntime::close(std::source_location sourceLocation) noexcept {
    if (head.next != &head) {
        fail("closing a QuickJS runtime with outstanding borrowers", sourceLocation, head);
    }
    JSRuntime* runtime = std::exchange(jsRuntime, nullptr);
    if (runtime != nullptr) {
        JS_FreeRuntime(runtime);
    }
    return {};
}

std::expected<QuickJSContext, std::error_code>
QuickJSContext::create(Borrowed<QuickJSRuntime> quickjsRuntime) noexcept {
    QuickJSContext quickjsContext;
    quickjsContext.jsContext = JS_NewContext(quickjsRuntime.get());
    if (quickjsContext.jsContext == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    quickjsContext.quickjsRuntime.emplace(std::move(quickjsRuntime));
    return std::expected<QuickJSContext, std::error_code>(std::in_place, std::move(quickjsContext));
}

QuickJSContext::~QuickJSContext() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("QuickJS context cleanup failed", std::source_location::current());
    }
}

QuickJSContext::QuickJSContext(QuickJSContext&& other) noexcept
    : quickjsRuntime(std::move(other.quickjsRuntime)), head(std::move(other.head)) {
    jsContext = std::exchange(other.jsContext, nullptr);
    other.quickjsRuntime.reset();
}

std::error_code QuickJSContext::close(std::source_location sourceLocation) noexcept {
    if (head.next != &head) {
        fail("closing a QuickJS context with outstanding borrowers", sourceLocation, head);
    }
    JSContext* context = std::exchange(jsContext, nullptr);
    if (context != nullptr) {
        JS_FreeContext(context);
        quickjsRuntime.reset();
    }
    return {};
}

QuickJSValue::QuickJSValue(Borrowed<QuickJSContext> context, JSValue value) noexcept {
    std::ignore = context.get();
    this->context.emplace(std::move(context));
    this->value = value;
    live = true;
}

QuickJSValue::~QuickJSValue() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("QuickJS value cleanup failed", std::source_location::current());
    }
}

QuickJSValue::QuickJSValue(QuickJSValue&& other) noexcept
    : context(std::move(other.context)), head(std::move(other.head)) {
    value = std::exchange(other.value, JS_UNDEFINED);
    live = std::exchange(other.live, false);
    other.context.reset();
}

JSValue QuickJSValue::release(std::source_location sourceLocation) noexcept {
    const JSValue result = get(sourceLocation);
    if (head.next != &head) {
        fail("releasing a QuickJS value with outstanding borrowers", sourceLocation, head);
    }
    value = JS_UNDEFINED;
    live = false;
    context.reset();
    return result;
}

std::error_code QuickJSValue::close(std::source_location sourceLocation) noexcept {
    if (head.next != &head) {
        fail("closing a QuickJS value with outstanding borrowers", sourceLocation, head);
    }
    if (!live) {
        return {};
    }
    live = false;
    const JSValue releasedValue = std::exchange(value, JS_UNDEFINED);
    if (!context.has_value()) {
        fail("QuickJS value has no context during cleanup", sourceLocation);
    }
    try {
        JS_FreeValue(context.value().get(sourceLocation), releasedValue);
    } catch (...) {
        fail("QuickJS value cleanup failed", sourceLocation);
    }
    context.reset();
    return {};
}

} // namespace amj::safe
