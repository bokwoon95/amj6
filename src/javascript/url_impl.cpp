#include "url_impl.hpp"

#include "javascript.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <ada/url_search_params.h>
#include <quickjs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amj::javascript {

JSValue urlParse(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext, "urlParse: requires at least 1 argument (%d provided)", argc);
        }

        // Get input string.
        if (!JS_IsString(argv[0])) {
            return JS_ThrowTypeError(jsContext, "urlParse: arg0: input not a string");
        }
        const std::optional<std::string> input = jsstring(jsContext, argv[0]);
        if (!input.has_value()) {
            return JS_EXCEPTION;
        }
        if (input.value().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlParse: arg0: input string too big");
        }

        std::optional<ada::url_aggregator> baseURLResult = std::nullopt;
        if (argc >= 2 && !JS_IsUndefined(argv[1])) {
            // Get base string.
            if (!JS_IsString(argv[1])) {
                return JS_ThrowTypeError(jsContext, "urlParse: arg1: base is not a string");
            }
            const std::optional<std::string> base = jsstring(jsContext, argv[1]);
            if (!base.has_value()) {
                return JS_EXCEPTION;
            }
            if (base.value().size() > (1U << 20U /* 1 MiB */)) {
                return JS_ThrowRangeError(jsContext, "urlParse: arg1: base string too big");
            }
            const ada::result<ada::url_aggregator> uri = ada::parse<ada::url_aggregator>(base.value(), nullptr);
            if (!uri.has_value()) {
                return JS_NULL;
            }
            baseURLResult = uri.value();
        }

        // Parse the URL.
        ada::result<ada::url_aggregator> urlResult;
        if (baseURLResult.has_value()) {
            urlResult = ada::parse<ada::url_aggregator>(input.value(), &baseURLResult.value());
        } else {
            urlResult = ada::parse<ada::url_aggregator>(input.value(), nullptr);
        }
        if (!urlResult.has_value()) {
            return JS_NULL;
        }
        if (urlResult.value().get_href().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlParse: url href too big");
        }
        const std::string_view search = urlResult.value().get_search();
        if (std::count(search.begin(), search.end(), '&') > 65535) {
            return JS_ThrowRangeError(jsContext, "urlParse: url queryString has too many entries");
        }

        // Return the URLParts.
        const ada::url_aggregator& url = urlResult.value();
        const std::map<std::string, JSONValue> urlParts = {
            {"href", std::string(url.get_href())},
            {"origin", url.get_origin()},
            {"protocol", std::string(url.get_protocol())},
            {"username", std::string(url.get_username())},
            {"password", std::string(url.get_password())},
            {"host", std::string(url.get_host())},
            {"hostname", std::string(url.get_hostname())},
            {"port", std::string(url.get_port())},
            {"pathname", std::string(url.get_pathname())},
            {"search", std::string(url.get_search())},
            {"hash", std::string(url.get_hash())},
        };
        return JSONValue(urlParts).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "urlParse: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "urlParse: unexpected native failure");
    }
}

JSValue urlWithComponent(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    try {
        if (argc < 3) {
            return JS_ThrowTypeError(jsContext, "urlWithComponent: requires 3 arguments (%d provided)", argc);
        }

        // Get href string.
        if (!JS_IsString(argv[0])) {
            return JS_ThrowTypeError(jsContext, "urlWithComponent: arg0: href not a string");
        }
        const std::optional<std::string> href = jsstring(jsContext, argv[0]);
        if (!href.has_value()) {
            return JS_EXCEPTION;
        }
        if (href.value().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlWithComponent: arg0: href string too big");
        }

        // Get component string.
        if (!JS_IsString(argv[1])) {
            return JS_ThrowTypeError(jsContext, "urlWithComponent: arg1: component not a string");
        }
        const std::optional<std::string> component = jsstring(jsContext, argv[1]);
        if (!component.has_value()) {
            return JS_EXCEPTION;
        }
        if (component.value().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlWithComponent: arg1: component string too big");
        }

        // Get value string.
        if (!JS_IsString(argv[2])) {
            return JS_ThrowTypeError(jsContext, "urlWithComponent: arg2: value not a string");
        }
        const std::optional<std::string> value = jsstring(jsContext, argv[2]);
        if (!value.has_value()) {
            return JS_EXCEPTION;
        }
        if (value.value().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlWithComponent: arg2: value string too big");
        }

        // Set the URL component.
        ada::result<ada::url_aggregator> urlResult = ada::parse<ada::url_aggregator>(href.value(), nullptr);
        if (!urlResult.has_value()) {
            return JS_NULL;
        }
        ada::url_aggregator& url = urlResult.value();
        if (component.value() == "href") {
            const bool ok = url.set_href(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "protocol") {
            const bool ok = url.set_protocol(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "username") {
            const bool ok = url.set_username(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "password") {
            const bool ok = url.set_password(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "host") {
            const bool ok = url.set_host(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "hostname") {
            const bool ok = url.set_hostname(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "port") {
            const bool ok = url.set_port(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "pathname") {
            const bool ok = url.set_pathname(value.value());
            if (!ok) {
                return JS_NULL;
            }
        } else if (component.value() == "search") {
            url.set_search(value.value());
        } else if (component.value() == "hash") {
            url.set_hash(value.value());
        } else {
            return JS_ThrowTypeError(jsContext, "urlWithComponent: arg1: unknown URL component");
        }
        if (url.get_href().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "urlWithComponent: url href too big");
        }
        const std::string_view search = url.get_search();
        if (std::count(search.begin(), search.end(), '&') > 65535) {
            return JS_ThrowRangeError(jsContext, "urlWithComponent: url queryString has too many entries");
        }

        // Return the URLParts.
        const std::map<std::string, JSONValue> urlParts = {
            {"href", std::string(url.get_href())},
            {"origin", url.get_origin()},
            {"protocol", std::string(url.get_protocol())},
            {"username", std::string(url.get_username())},
            {"password", std::string(url.get_password())},
            {"host", std::string(url.get_host())},
            {"hostname", std::string(url.get_hostname())},
            {"port", std::string(url.get_port())},
            {"pathname", std::string(url.get_pathname())},
            {"search", std::string(url.get_search())},
            {"hash", std::string(url.get_hash())},
        };
        return JSONValue(urlParts).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "urlWithComponent: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "urlWithComponent: unexpected native failure");
    }
}

JSValue queryStringParse(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext, "queryStringParse: requires 1 argument (%d provided)", argc);
        }

        // Get queryString string.
        if (!JS_IsString(argv[0])) {
            return JS_ThrowTypeError(jsContext, "queryStringParse: arg0: queryString not a string");
        }
        const std::optional<std::string> queryString = jsstring(jsContext, argv[0]);
        if (!queryString.has_value()) {
            return JS_EXCEPTION;
        }
        if (queryString.value().size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "queryStringParse: arg0: queryString too big");
        }
        if (std::count(queryString.value().begin(), queryString.value().end(), '&') > 65535) {
            return JS_ThrowRangeError(jsContext, "queryStringParse: arg0: queryString has too many entries");
        }

        // Return the URLSearchParams entries.
        const ada::url_search_params urlSearchParams(queryString.value());
        std::vector<JSONValue> entries;
        for (const std::pair<std::string, std::string>& entry : urlSearchParams) {
            entries.push_back(JSONValue(std::vector<JSONValue>{entry.first, entry.second}));
        }
        return JSONValue(entries).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "queryStringParse: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "queryStringParse: unexpected native failure");
    }
}

JSValue queryStringify(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext, "queryStringify: requires 1 argument (%d provided)", argc);
        }

        // Get entries array.
        const JSValueConst entries = argv[0];
        if (!JS_IsArray(entries)) {
            return JS_ThrowTypeError(jsContext, "queryStringify: arg0: entries not an array");
        }
        int64_t count = 0;
        if (JS_GetLength(jsContext, entries, &count) < 0) {
            return JS_EXCEPTION;
        }
        if (count > 65535) {
            return JS_ThrowRangeError(jsContext, "queryStringify: arg0: queryString has too many entries");
        }
        const size_t countSize = static_cast<size_t>(count);

        ada::url_search_params urlSearchParams;
        std::size_t numBytes = 0;
        for (size_t index = 0; index < countSize; index++) {
            // Get entry array.
            const ScopedJSValue entryJSValue(jsContext, JS_GetPropertyUint32(jsContext, entries, static_cast<uint32_t>(index)));
            if (JS_IsException(entryJSValue.value)) {
                return JS_EXCEPTION;
            }
            if (!JS_IsArray(entryJSValue.value)) {
                return JS_ThrowTypeError(jsContext, "queryStringify: arg0: entries[%zu] not an array", index);
            }
            int64_t length = 0;
            if (JS_GetLength(jsContext, entryJSValue.value, &length) < 0) {
                return JS_EXCEPTION;
            }
            if (length != 2) {
                return JS_ThrowTypeError(jsContext, "queryStringify: arg0: entries[%zu] requires 2 elements (%lld provided)", index, static_cast<long long>(length));
            }

            // Get name string.
            const ScopedJSValue nameJSValue(jsContext, JS_GetPropertyUint32(jsContext, entryJSValue.value, 0));
            if (JS_IsException(nameJSValue.value)) {
                return JS_EXCEPTION;
            }
            if (!JS_IsString(nameJSValue.value)) {
                return JS_ThrowTypeError(jsContext, "queryStringify: arg0: entries[%zu][0] must be a string", index);
            }
            const std::optional<std::string> name = jsstring(jsContext, nameJSValue.value);
            if (!name.has_value()) {
                return JS_EXCEPTION;
            }
            if (name.value().size() > (1U << 20U /* 1 MiB */)) {
                return JS_ThrowRangeError(jsContext, "queryStringify: arg0: entries[%zu][0] string is too big", index);
            }

            // Get value string.
            const ScopedJSValue valueJSValue(jsContext, JS_GetPropertyUint32(jsContext, entryJSValue.value, 1));
            if (JS_IsException(valueJSValue.value)) {
                return JS_EXCEPTION;
            }
            if (!JS_IsString(valueJSValue.value)) {
                return JS_ThrowTypeError(jsContext, "queryStringify: arg0: entries[%zu][1] must be a string", index);
            }
            const std::optional<std::string> value = jsstring(jsContext, valueJSValue.value);
            if (!value.has_value()) {
                return JS_EXCEPTION;
            }
            if (value.value().size() > (1U << 20U /* 1 MiB */)) {
                return JS_ThrowRangeError(jsContext, "queryStringify: arg0: entries[%zu][1] string is too big", index);
            }

            if (numBytes + name.value().size() + value.value().size() > (1U << 20U /* 1 MiB */)) {
                return JS_ThrowRangeError(jsContext, "queryStringify: arg0: queryString is too big");
            }
            numBytes = numBytes + name.value().size() + value.value().size();
            urlSearchParams.append(name.value(), value.value());
        }

        // Return the queryString.
        const std::string queryString = urlSearchParams.to_string();
        if (queryString.size() > (1U << 20U /* 1 MiB */)) {
            return JS_ThrowRangeError(jsContext, "queryStringify: queryString is too big");
        }
        return JSONValue(queryString).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "queryStringify: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "queryStringify: unexpected native failure");
    }
}

JSModuleDef* createURLImplModule(JSContext* jsContext) {
    try {
        static const std::vector<JSCFunctionListEntry> functionList = {
            JS_CFUNC_DEF("urlParse", 1, urlParse),
            JS_CFUNC_DEF("urlWithComponent", 3, urlWithComponent),
            JS_CFUNC_DEF("queryStringParse", 1, queryStringParse),
            JS_CFUNC_DEF("queryStringify", 1, queryStringify),
        };
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:url_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            return JS_SetModuleExportList(jsContext, jsModuleDef, functionList.data(), static_cast<int>(functionList.size()));
        });
        if (jsModuleDef == nullptr) {
            return nullptr;
        }
        if (JS_AddModuleExportList(jsContext, jsModuleDef, functionList.data(), static_cast<int>(functionList.size())) < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(jsContext);
        return nullptr;
    }
}

} // namespace amj::javascript
