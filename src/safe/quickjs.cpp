#include "quickjs.hpp"

#include "safe.hpp"

#include <quickjs.h>

#include <new>
#include <source_location>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {

QuickJSRuntime::QuickJSRuntime() {
    this->jsRuntime = JS_NewRuntime();
    if (this->jsRuntime == nullptr) {
        throw std::bad_alloc();
    }
}

QuickJSRuntime::~QuickJSRuntime() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("QuickJS runtime cleanup failed", std::source_location::current());
    }
}

QuickJSRuntime::QuickJSRuntime(QuickJSRuntime&& other) noexcept : head(std::move(other.head)) {
    this->jsRuntime = std::exchange(other.jsRuntime, nullptr);
}

JSRuntime* QuickJSRuntime::get(std::source_location sourceLocation) const noexcept {
    if (jsRuntime == nullptr) {
        fail("access through a closed or moved QuickJS runtime", sourceLocation);
    }
    return jsRuntime;
}

Borrowed<QuickJSRuntime> QuickJSRuntime::borrow(std::source_location sourceLocation) noexcept {
    return Borrowed<QuickJSRuntime>(get(sourceLocation), head, sourceLocation);
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

QuickJSContext::QuickJSContext(Borrowed<QuickJSRuntime> quickjsRuntime) {
    this->jsContext = JS_NewContext(quickjsRuntime.get());
    if (this->jsContext == nullptr) {
        throw std::bad_alloc();
    }
    this->quickjsRuntime.emplace(std::move(quickjsRuntime));
}

QuickJSContext::~QuickJSContext() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("QuickJS context cleanup failed", std::source_location::current());
    }
}

QuickJSContext::QuickJSContext(QuickJSContext&& other) noexcept
    : quickjsRuntime(std::move(other.quickjsRuntime)), head(std::move(other.head)) {
    this->jsContext = std::exchange(other.jsContext, nullptr);
    other.quickjsRuntime.reset();
}

JSContext* QuickJSContext::get(std::source_location location) const noexcept {
    if (jsContext == nullptr) {
        fail("access through a closed or moved QuickJS context", location);
    }
    return jsContext;
}

Borrowed<QuickJSContext> QuickJSContext::borrow(std::source_location location) noexcept {
    return Borrowed<QuickJSContext>(get(location), head, location);
}

std::error_code QuickJSContext::close(std::source_location location) noexcept {
    if (head.next != &head) {
        fail("closing a QuickJS context with outstanding borrowers", location, head);
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
    this->live = true;
}

QuickJSValue::~QuickJSValue() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("QuickJS value cleanup failed", std::source_location::current());
    }
}

QuickJSValue::QuickJSValue(QuickJSValue&& other) noexcept
    : context(std::move(other.context)), head(std::move(other.head)) {
    this->value = std::exchange(other.value, JS_UNDEFINED);
    this->live = std::exchange(other.live, false);
    other.context.reset();
}

JSValue QuickJSValue::get(std::source_location location) const noexcept {
    if (!live) {
        fail("access through a closed or moved QuickJS value", location);
    }
    return value;
}

Borrowed<QuickJSValue> QuickJSValue::borrow(std::source_location location) noexcept {
    return Borrowed<QuickJSValue>(get(location), head, location);
}

JSValue QuickJSValue::release(std::source_location location) noexcept {
    const JSValue result = get(location);
    if (head.next != &head) {
        fail("releasing a QuickJS value with outstanding borrowers", location, head);
    }
    value = JS_UNDEFINED;
    live = false;
    context.reset();
    return result;
}

std::error_code QuickJSValue::close(std::source_location location) noexcept {
    if (head.next != &head) {
        fail("closing a QuickJS value with outstanding borrowers", location, head);
    }
    if (!live) {
        return {};
    }
    live = false;
    const JSValue releasedValue = std::exchange(value, JS_UNDEFINED);
    if (!context.has_value()) {
        fail("QuickJS value has no context during cleanup", location);
    }
    try {
        JS_FreeValue(context.value().get(location), releasedValue);
    } catch (...) {
        fail("QuickJS value cleanup failed", location);
    }
    context.reset();
    return {};
}

} // namespace amj::safe
