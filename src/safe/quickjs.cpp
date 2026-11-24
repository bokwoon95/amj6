#include "quickjs.hpp"

#include "safe.hpp"

#include <quickjs.h>

#include <memory>
#include <system_error>
#include <tuple>

namespace amj::safe {

OwnedJSValue::OwnedJSValue(const Borrowed<JSContext*>& jsContext, JSValue jsValue) {
    try {
        lifetime = std::make_shared<Lifetime>();
        lifetime->creationTrace = captureTrace();
        this->jsContext = jsContext;
        this->jsValue = jsValue;
    } catch (...) {
        JS_FreeValue(jsContext.get(), jsValue);
        throw;
    }
}

OwnedJSValue::~OwnedJSValue() noexcept {
    std::ignore = close();
}

JSValue OwnedJSValue::get() const {
    if (lifetime == nullptr || lifetime->state != ResourceState::Live) {
        fail("access through a closed or moved JS value", lifetime.get());
    }
    if (!jsContext.has_value()) {
        fail("JS value has no context", lifetime.get());
    }
    std::ignore = jsContext.value().get();
    return jsValue;
}

JSValue OwnedJSValue::release() {
    const JSValue result = get();
    lifetime->state = ResourceState::Closed;
    lifetime->closureTrace = captureTrace();
    jsValue = JS_UNDEFINED;
    jsContext.reset();
    return result;
}

std::error_code OwnedJSValue::close() noexcept {
    if (lifetime == nullptr || lifetime->state == ResourceState::Closed) {
        return {};
    }
    if (lifetime->state == ResourceState::Closing) {
        fail("reentrant JS value closure", lifetime.get());
    }
    lifetime->state = ResourceState::Closing;
    lifetime->closureTrace = captureTrace();
    if (!jsContext.has_value()) {
        fail("JS value has no context during closure", lifetime.get());
    }
    try {
        JS_FreeValue(jsContext.value().get(), jsValue);
    } catch (...) {
        fail("JS value cleanup threw", lifetime.get());
    }
    jsValue = JS_UNDEFINED;
    jsContext.reset();
    lifetime->state = ResourceState::Closed;
    return {};
}

} // namespace amj::safe
