#include "fetch_impl.hpp"

#include "errors/errors.hpp"
#include "integers/integers.hpp"
#include "javascript.hpp"
#include "owned/quickjs.hpp"
#include "owned/uv.hpp"
#include "void_ptr/void_ptr.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/header.h>
#include <curl/multi.h>
#include <curl/system.h>
#include <quickjs.h>
#include <uv.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace errors = amj::errors;
namespace integers = amj::integers;
namespace owned = amj::owned;
namespace this_thread = std::this_thread;
namespace void_ptr = amj::void_ptr;

namespace {

constexpr std::size_t bodyBufferLimit = 256U << 10U /* 256 KiB */;

// Retain native codes without allocating diagnostic strings in callbacks.
class UvErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "libuv";
    }
    std::string message(int code) const override {
        return uv_strerror(code);
    }
};

// Startup allocation failure may terminate.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const UvErrorCategory uvErrorCategory;

class CurlErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "curl";
    }
    std::string message(int code) const override {
        return curl_easy_strerror(static_cast<CURLcode>(code));
    }
};

// Startup allocation failure may terminate.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const CurlErrorCategory curlErrorCategory;

class CurlMErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "curlm";
    }
    std::string message(int code) const override {
        return curl_multi_strerror(static_cast<CURLMcode>(code));
    }
};

// Startup allocation failure may terminate.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const CurlMErrorCategory curlMErrorCategory;

std::mutex classIDMutex = {};

// Startup allocation failure may terminate.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
std::map<const JSRuntime*, JSClassID> fetchClientClassIDMap = {};

// Startup allocation failure may terminate.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
std::map<const JSRuntime*, JSClassID> fetchResponseClassIDMap = {};

} // namespace

std::optional<FetchRequest> FetchRequest::create(JSContext& jsContext, JSValueConst jsValue) {
    const std::string_view httpTokenChars =
        "!#$%&'*+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ^_`abcdefghijklmnopqrstuvwxyz|~";
    if (!JS_IsObject(jsValue)) {
        JS_ThrowTypeError(&jsContext, "arg0: not an object");
        return std::nullopt;
    }
    FetchRequest fetchRequest;

    // Get method string.
    owned::JSValue methodJSValue(&jsContext, JS_GetPropertyStr(&jsContext, jsValue, "method"));
    if (JS_IsException(methodJSValue.get())) {
        return std::nullopt;
    }
    if (JS_IsUndefined(methodJSValue.get())) {
        fetchRequest.method = "GET";
    } else {
        const std::optional<std::string> method = jsToString(&jsContext, methodJSValue.get());
        if (!method.has_value()) {
            return std::nullopt;
        }
        if (method.value().empty() ||
            method.value().find_first_not_of(httpTokenChars) != std::string::npos) {
            JS_ThrowTypeError(&jsContext, "method is invalid");
            return std::nullopt;
        }
        if (equalFoldASCII(method.value(), "CONNECT") || equalFoldASCII(method.value(), "TRACE") ||
            equalFoldASCII(method.value(), "TRACK")) {
            JS_ThrowTypeError(&jsContext, "method is invalid");
            return std::nullopt;
        }
        fetchRequest.method = method.value();
    }

    // Get url string.
    const std::optional<std::string> url = jsToString(&jsContext,
        owned::JSValue(&jsContext, JS_GetPropertyStr(&jsContext, jsValue, "url")).get());
    if (!url.has_value()) {
        return std::nullopt;
    }
    if (url.value().empty() || url.value().size() > (2U << 20U /* 2 MiB */) ||
        url.value().find('\0') != std::string::npos) {
        JS_ThrowTypeError(&jsContext, "url is invalid");
        return std::nullopt;
    }
    ada::result<ada::url_aggregator> adaURL = ada::parse<ada::url_aggregator>(url.value());
    if (!adaURL.has_value()) {
        JS_ThrowTypeError(&jsContext, "url is invalid");
        return std::nullopt;
    }
    if (adaURL.value().get_protocol() != "http:" && adaURL.value().get_protocol() != "https:") {
        JS_ThrowTypeError(&jsContext, "url protocol not supported");
        return std::nullopt;
    }
    if (!adaURL.value().get_username().empty() || !adaURL.value().get_password().empty()) {
        JS_ThrowTypeError(&jsContext, "url credentials not supported");
        return std::nullopt;
    }
    fetchRequest.url = url.value();

    // Get headers array.
    const owned::JSValue headersJSValue(&jsContext,
        JS_GetPropertyStr(&jsContext, jsValue, "headers"));
    if (JS_IsException(headersJSValue.get())) {
        return std::nullopt;
    }
    uint32_t connectionHeaderIndex = static_cast<uint32_t>(-1);
    uint32_t contentLengthHeaderIndex = static_cast<uint32_t>(-1);
    std::size_t contentLength = 0;
    if (!JS_IsUndefined(headersJSValue.get())) {
        if (!JS_IsArray(headersJSValue.get())) {
            JS_ThrowTypeError(&jsContext, "headers not an array");
            return std::nullopt;
        }
        const std::optional<uint32_t> headersLength = jsToUint32(&jsContext,
            owned::JSValue(&jsContext,
                JS_GetPropertyStr(&jsContext, headersJSValue.get(), "length"))
                .get());
        if (!headersLength.has_value()) {
            return std::nullopt;
        }
        for (uint32_t index = 0; index < headersLength.value(); index++) {
            const owned::JSValue headerJSValue(&jsContext,
                JS_GetPropertyUint32(&jsContext, headersJSValue.get(), index));
            if (JS_IsException(headerJSValue.get())) {
                return std::nullopt;
            }
            if (!JS_IsArray(headerJSValue.get())) {
                JS_ThrowTypeError(&jsContext, "headers[%lu] not an array",
                    static_cast<unsigned long>(index));
                return std::nullopt;
            }
            // Make sure header array has at least two elements.
            const std::optional<uint32_t> headerLength = jsToUint32(&jsContext,
                owned::JSValue(&jsContext,
                    JS_GetPropertyStr(&jsContext, headerJSValue.get(), "length"))
                    .get());
            if (!headerLength.has_value()) {
                return std::nullopt;
            }
            if (headerLength.value() < 2) {
                JS_ThrowTypeError(&jsContext, "headers[%lu] length must be at least 2 (is %lu)",
                    static_cast<unsigned long>(index),
                    static_cast<unsigned long>(headerLength.value()));
                return std::nullopt;
            }
            // Get header name.
            const std::optional<std::string> headerName = jsToString(&jsContext,
                owned::JSValue(&jsContext, JS_GetPropertyUint32(&jsContext, headerJSValue.get(), 0))
                    .get());
            if (!headerName.has_value()) {
                return std::nullopt;
            }
            if (headerName.value().empty() ||
                headerName.value().find_first_not_of(httpTokenChars) != std::string::npos) {
                JS_ThrowTypeError(&jsContext, "headers[%lu]: invalid header name",
                    static_cast<unsigned long>(index));
                return std::nullopt;
            }
            if (equalFoldASCII(headerName.value(), "Transfer-Encoding") ||
                equalFoldASCII(headerName.value(), "Keep-Alive") ||
                equalFoldASCII(headerName.value(), "Upgrade") ||
                equalFoldASCII(headerName.value(), "Expect")) {
                JS_ThrowTypeError(&jsContext, "headers[%lu]: forbidden header name: %s",
                    static_cast<unsigned long>(index), headerName.value().c_str());
                return std::nullopt;
            }
            // Get header value.
            const std::optional<std::string> headerValue = jsToString(&jsContext,
                owned::JSValue(&jsContext, JS_GetPropertyUint32(&jsContext, headerJSValue.get(), 1))
                    .get());
            if (!headerValue.has_value()) {
                return std::nullopt;
            }
            for (const char character : headerValue.value()) {
                const unsigned char c = static_cast<unsigned char>(character);
                // Permit visible ASCII characters.
                if (c == ' ' || c == '\t' || (c >= 0x21 && c <= 0x7E)) {
                    continue;
                }
                // Permit non-ASCII bytes.
                if (c >= 0x80 && c <= 0xFF) {
                    continue;
                }
                JS_ThrowTypeError(&jsContext, "headers[%lu]: invalid header value",
                    static_cast<unsigned long>(index));
                return std::nullopt;
            }
            // Trim header value.
            std::string_view trimmedHeaderValue;
            const std::size_t i = headerValue.value().find_first_not_of(" \t");
            if (i != std::string::npos) {
                const std::size_t j = headerValue.value().find_last_not_of(" \t");
                trimmedHeaderValue = std::string_view(headerValue.value()).substr(i, j - i + 1);
            }
            // Validate Connection header.
            if (equalFoldASCII(headerName.value(), "Connection")) {
                if (connectionHeaderIndex != static_cast<uint32_t>(-1)) {
                    JS_ThrowTypeError(&jsContext,
                        "headers[%lu]: Connection header specified more than once (previously at "
                        "headers[%lu])",
                        static_cast<unsigned long>(index),
                        static_cast<unsigned long>(connectionHeaderIndex));
                    return std::nullopt;
                }
                if (!equalFoldASCII(trimmedHeaderValue, "close") &&
                    !equalFoldASCII(trimmedHeaderValue, "keep-alive")) {
                    JS_ThrowTypeError(&jsContext,
                        "headers[%lu]: Connection must be close or keep-alive",
                        static_cast<unsigned long>(index));
                    return std::nullopt;
                }
                connectionHeaderIndex = index;
            }
            // Validate Content-Length header.
            if (equalFoldASCII(headerName.value(), "Content-Length")) {
                if (contentLengthHeaderIndex != static_cast<uint32_t>(-1)) {
                    JS_ThrowTypeError(&jsContext,
                        "headers[%lu]: Content-Length header specified more than once (previously "
                        "at headers[%lu])",
                        static_cast<unsigned long>(index),
                        static_cast<unsigned long>(contentLengthHeaderIndex));
                    return std::nullopt;
                }
                if (trimmedHeaderValue.empty() ||
                    trimmedHeaderValue.find_first_not_of("0123456789") != std::string_view::npos) {
                    JS_ThrowTypeError(&jsContext,
                        "headers[%lu]: Content-Length header value must be a nonnegative decimal "
                        "integer",
                        static_cast<unsigned long>(index));
                    return std::nullopt;
                }
                std::size_t num = 0;
                const std::from_chars_result fromCharsResult =
                    std::from_chars(trimmedHeaderValue.data(),
                        trimmedHeaderValue.data() + trimmedHeaderValue.size(), num);
                if (fromCharsResult.ec != std::errc() ||
                    fromCharsResult.ptr != trimmedHeaderValue.data() + trimmedHeaderValue.size()) {
                    JS_ThrowTypeError(&jsContext, "Content-Length is out of range");
                    return std::nullopt;
                }
                contentLengthHeaderIndex = index;
                contentLength = num;
            }
            fetchRequest.headers.push_back({headerName.value(), headerValue.value()});
        }
    }

    // Get body string or byte array.
    const owned::JSValue bodyJSValue(&jsContext, JS_GetPropertyStr(&jsContext, jsValue, "body"));
    if (JS_IsException(bodyJSValue.get())) {
        return std::nullopt;
    }
    if (!JS_IsUndefined(bodyJSValue.get())) {
        if (equalFoldASCII(fetchRequest.method, "GET") ||
            equalFoldASCII(fetchRequest.method, "HEAD")) {
            JS_ThrowTypeError(&jsContext, "body is not allowed for GET/HEAD requests");
            return std::nullopt;
        }
        if (JS_IsString(bodyJSValue.get())) {
            const std::optional<std::string> body = jsToString(&jsContext, bodyJSValue.get());
            if (!body.has_value()) {
                return std::nullopt;
            }
            if (body.value().size() > (32U << 20U /* 32 MiB */)) {
                JS_ThrowRangeError(&jsContext, "body exceeds 32 MiB");
                return std::nullopt;
            }
            fetchRequest.body.assign(body.value().begin(), body.value().end());
        } else if (JS_GetTypedArrayType(bodyJSValue.get()) == JS_TYPED_ARRAY_UINT8) {
            std::size_t offset = 0;
            std::size_t length = 0;
            const owned::JSValue arrayBufferJSValue(&jsContext,
                JS_GetTypedArrayBuffer(&jsContext, bodyJSValue.get(), &offset, &length, nullptr));
            if (JS_IsException(arrayBufferJSValue.get())) {
                return std::nullopt;
            }
            if (!JS_IsArrayBuffer(arrayBufferJSValue.get())) {
                JS_ThrowTypeError(&jsContext,
                    "body cannot be backed by a SharedArrayBuffer (only ArrayBuffer is allowed)");
                return std::nullopt;
            }
            std::size_t size = 0;
            const uint8_t* arrayBuffer =
                JS_GetArrayBuffer(&jsContext, &size, arrayBufferJSValue.get());
            if (arrayBuffer == nullptr) {
                if (JS_HasException(&jsContext)) {
                    return std::nullopt;
                }
            }
            if (integers::addOverflows(offset, length) || offset + length > size) {
                JS_ThrowInternalError(&jsContext, "body: QuickJS bounds violations");
                return std::nullopt;
            }
            if (length > 0) {
                if (length > (32U << 20U /* 32 MiB */)) {
                    JS_ThrowRangeError(&jsContext,
                        "Invalid request body bounds or upload exceeds 32 MiB");
                    return std::nullopt;
                }
                if (arrayBuffer == nullptr) {
                    JS_ThrowInternalError(&jsContext, "body is missing underlying ArrayBuffer");
                    return std::nullopt;
                }
                fetchRequest.body.assign(arrayBuffer + offset, arrayBuffer + offset + length);
            }
        } else {
            JS_ThrowTypeError(&jsContext, "body property must be a string or Uint8Array");
            return std::nullopt;
        }
    }

    if (contentLengthHeaderIndex != static_cast<uint32_t>(-1)) {
        if (contentLength != fetchRequest.body.size()) {
            JS_ThrowTypeError(&jsContext,
                "Content-Length does not match the request body byte length");
            return std::nullopt;
        }
    }
    return fetchRequest;
}

// Passed as CURLOPT_HEADERFUNCTION to read the response headers.
// NOLINTNEXTLINE(misc-const-correctness): curl_write_callback requires char*.
std::size_t FetchState::curlHeaderFunction(char* buffer, std::size_t size, std::size_t nitems,
    void* userdata) noexcept {
    try {
        FetchState& fetchState = *static_cast<void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
        if (integers::multiplyOverflows(size, nitems)) {
            return CURL_WRITEFUNC_ERROR;
        }
        const std::size_t realsize = size * nitems;
        const std::string_view line(buffer, realsize);

        // If responseHeadersComplete, we are handling response trailers.
        if (fetchState.responseHeadersComplete) {
            if (integers::addOverflows(fetchState.responseTrailersSize, realsize)) {
                return CURL_WRITEFUNC_ERROR;
            }
            if (fetchState.responseTrailersSize + realsize > (4U << 10U /* 4 KiB */)) {
                fetchState.error = errors::message("Response trailers exceed 4 KiB");
                return CURL_WRITEFUNC_ERROR;
            }
            fetchState.responseTrailersSize += realsize;
            return realsize;
        }

        // Every fresh response following a redirect gets a new responseHeadersSize
        // budget.
        if (line.starts_with("HTTP/") && fetchState.responseRedirecting) {
            fetchState.responseHeadersSize = 0;
        }
        if (integers::addOverflows(fetchState.responseHeadersSize, realsize)) {
            return CURL_WRITEFUNC_ERROR;
        }
        if (fetchState.responseHeadersSize + realsize > (10U << 20U /* 10 MiB */)) {
            fetchState.error = errors::message("Response headers exceed 10 MiB");
            return CURL_WRITEFUNC_ERROR;
        }
        fetchState.responseHeadersSize += realsize;

        // If it's a fresh response, reset the responseHeaders and the
        // responseRedirecting flag.
        if (line.starts_with("HTTP/")) {
            fetchState.responseHeaders.clear();
            fetchState.responseRedirecting = false;
            return realsize;
        }

        // Response headers have not yet completed.
        if (line != "\r\n" && line != "\n") {
            return realsize;
        }

        // If we reach here, response headers have completed. Get the responseCode.
        long responseCode = 0;
        CURLcode curlCode = curl_easy_getinfo(fetchState.ownedCurlEasyHandle,
            CURLINFO_RESPONSE_CODE, &responseCode);
        if (curlCode != CURLE_OK) {
            fetchState.error = errors::message(curl_easy_strerror(curlCode));
            return CURL_WRITEFUNC_ERROR;
        }

        // If it's informational 1XX, ignore the responseCode.
        if (responseCode >= 100 && responseCode < 200) {
            if (responseCode == 101) {
                fetchState.error = errors::message("HTTP protocol upgrades are not supported");
                return CURL_WRITEFUNC_ERROR;
            }
            return realsize;
        }

        // If it's a redirect 3XX and the Location header is present, ignore the
        // responseCode and set the responseRedirecting flag.
        if (responseCode == 301 || responseCode == 302 || responseCode == 303 ||
            responseCode == 307 || responseCode == 308) {
            curl_header* curlHeader = nullptr;
            const CURLHcode curlHCode = curl_easy_header(fetchState.ownedCurlEasyHandle, "location",
                0, CURLH_HEADER, -1, &curlHeader);
            if (curlHCode != CURLHE_OK) {
                if (curlHCode != CURLHE_MISSING && curlHCode != CURLHE_NOHEADERS) {
                    fetchState.error = errors::message("Could not obtain response redirect header");
                    return CURL_WRITEFUNC_ERROR;
                }
            }
            if (curlHeader != nullptr && curlHeader->value[0] != '\0') {
                fetchState.responseRedirecting = true;
                return realsize;
            }
        }

        fetchState.responseCode = responseCode;

        // Get the effectiveURL.
        // CURLINFO_EFFECTIVE_URL requires char**.
        // NOLINTNEXTLINE(misc-const-correctness)
        char* effectiveURL = nullptr;
        curlCode = curl_easy_getinfo(fetchState.ownedCurlEasyHandle, CURLINFO_EFFECTIVE_URL,
            &effectiveURL);
        if (curlCode != CURLE_OK) {
            fetchState.error = errors::message("Could not obtain response URL");
            return CURL_WRITEFUNC_ERROR;
        }
        if (effectiveURL != nullptr) {
            fetchState.effectiveURL = effectiveURL;
        }

        // Get the responseHeaders.
        curl_header* curlHeader = nullptr;
        while (true) {
            curlHeader =
                curl_easy_nextheader(fetchState.ownedCurlEasyHandle, CURLH_HEADER, -1, curlHeader);
            if (curlHeader == nullptr) {
                break;
            }
            std::string name(curlHeader->name);
            const std::string value(curlHeader->value);
            for (char& ch : name) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            fetchState.responseHeaders.push_back({name, value});
        }
        fetchState.responseHeadersComplete = true;
        return realsize;
    } catch (...) {
        return static_cast<std::size_t>(CURL_WRITEFUNC_ERROR);
    }
}

// Passed as CURLOPT_WRITEFUNCTION to read the response body.
std::size_t FetchState::curlWriteFunction(char* ptr, std::size_t size, std::size_t nmemb,
    void* userdata) noexcept {
    try {
        FetchState& fetchState =
            *static_cast<const void_ptr::VoidPtr*>(userdata)->cast<FetchState*>();
        if (integers::multiplyOverflows(size, nmemb)) {
            return CURL_WRITEFUNC_ERROR;
        }
        const std::size_t realsize = size * nmemb;

        // Body is empty.
        if (realsize == 0) {
            return 0;
        }

        // Ignore the body if we are redirecting.
        if (fetchState.responseRedirecting) {
            return realsize;
        }

        // If the responseBufferSize exceeds the bodyBufferLimit, pause curl
        // transfers.
        if (integers::addOverflows(fetchState.responseBufferSize, realsize)) {
            return CURL_WRITEFUNC_ERROR;
        }
        if (fetchState.responseBufferSize + realsize > bodyBufferLimit) {
            fetchState.responseCurlTransferStatus = CurlTransferStatus::Paused;
            return CURL_WRITEFUNC_PAUSE;
        }

        // Else append the bytes to the responseBuffer.
        fetchState.responseBuffer.push_back(std::vector<uint8_t>(ptr, ptr + realsize));
        fetchState.responseBufferSize += realsize;
        return realsize;
    } catch (...) {
        return static_cast<std::size_t>(CURL_WRITEFUNC_ERROR);
    }
}

bool FetchClient::triggerPromiseSettlement() {
    if (pendingError.hasValue()) {
        return false;
    }
    uv_ref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle.get()));
    const int status = uv_async_send(uvAsyncHandle.get());
    if (status != 0) {
        uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle.get()));
        if (!pendingError.hasValue()) {
            pendingError = errors::code(status, uvErrorCategory);
        }
        return false;
    }
    return true;
}

bool FetchClient::driveCurlTransfers(curl_socket_t socket, int eventBitmask) {
    // Check for pendingErrors before we call curl_multi_socket_action().
    if (pendingError.hasValue()) {
        return false;
    }

    // Tell curl to read and write socket data.
    int runningHandles = 0;
    const CURLMcode curlMCode =
        curl_multi_socket_action(ownedCurlMultiHandle, socket, eventBitmask, &runningHandles);
    if (curlMCode != CURLM_OK) {
        if (!pendingError.hasValue()) {
            pendingError = errors::code(curlMCode, curlMErrorCategory);
        }
        return false;
    }

    // curl_multi_socket_action() may invoke curlSocketFunction or
    // curlTimerFunction, which may have set pendingError. Check before
    // proceeding.
    if (pendingError.hasValue()) {
        return false;
    }

    // Mark the fetchStates that have completed.
    while (true) {
        int msgsInQueue = 0;
        const CURLMsg* curlMsg = curl_multi_info_read(ownedCurlMultiHandle, &msgsInQueue);
        if (curlMsg == nullptr) {
            break;
        }
        if (curlMsg->msg != CURLMSG_DONE) {
            continue;
        }
        if (!fetchStateMap.contains(curlMsg->easy_handle)) {
            std::terminate(); // A completed transfer must have registered callback storage.
        }
        FetchState& fetchState = fetchStateMap.at(curlMsg->easy_handle);
        fetchState.responseCurlTransferStatus = CurlTransferStatus::Completed;
        if (curlMsg->data.result != CURLE_OK) {
            fetchState.error = errors::code(curlMsg->data.result, curlErrorCategory);
        } else if (!fetchState.responseHeadersComplete) {
            fetchState.error = errors::message("HTTP response ended without final headers");
        }
    }

    return triggerPromiseSettlement();
}

// curl calls this function to specify which socket events should trigger
// driveCurlTransfers().
//
// 1. CURL_POLL_NONE: Stop scheduling driveCurlTransfers() for socket events,
// retaining the poll handle.
//
// 2. CURL_POLL_IN: Call driveCurlTransfers() when this socket becomes readable.
//
// 3. CURL_POLL_OUT: Call driveCurlTransfers() when this socket becomes writable.
//
// 4. CURL_POLL_INOUT: Call driveCurlTransfers() when this socket becomes
// readable or writable.
//
// 5. CURL_POLL_REMOVE: Stop scheduling driveCurlTransfers() for socket events
// and close the poll handle.
int FetchClient::curlMSocketFunction(CURL* /* curlEasyHandle */, curl_socket_t socket, int what,
    void* clientp, void* /* socketp */) noexcept {
    struct PollData {
        FetchClient& fetchClient;
        curl_socket_t socket = CURL_SOCKET_BAD;
    };
    FetchClient& fetchClient =
        *static_cast<const void_ptr::VoidPtr*>(clientp)->cast<FetchClient*>();
    try {
        // Stop watching this socket and close its poll handle.
        if (what == CURL_POLL_REMOVE) {
            fetchClient.uvPollHandleMap.erase(socket);
            return 0;
        }

        // Get the uvPollHandle associated with this socket, creating it if not
        // exists.
        if (!fetchClient.uvPollHandleMap.contains(socket)) {
            owned::UvHandle<uv_poll_t> ownedUvPollHandle(std::make_unique<uv_poll_t>());
            const int status =
                uv_poll_init_socket(&fetchClient.uvLoop, ownedUvPollHandle.get(), socket);
            if (status != 0) {
                if (!fetchClient.pendingError.hasValue()) {
                    fetchClient.pendingError = errors::code(status, uvErrorCategory);
                }
                return -1;
            }
            ownedUvPollHandle.setInitialized();
            ownedUvPollHandle.get()->data =
                new void_ptr::VoidPtr(PollData{
                    .fetchClient = fetchClient,
                    .socket = socket,
                });
            fetchClient.uvPollHandleMap.insert_or_assign(socket, std::move(ownedUvPollHandle));
        }
        uv_poll_t& uvPollHandle = *fetchClient.uvPollHandleMap.at(socket).get();

        // Stop watching this socket, retaining its poll handle.
        if (what == CURL_POLL_NONE) {
            const int status = uv_poll_stop(&uvPollHandle);
            if (status != 0) {
                if (!fetchClient.pendingError.hasValue()) {
                    fetchClient.pendingError = errors::code(status, uvErrorCategory);
                }
                return -1;
            }
            return 0;
        }

        // libuv calls uvPollCallback when the socket is readable, writable, or
        // encounters an error.
        const auto uvPollCallback = [](uv_poll_t* uvPollHandle, int status, int events) noexcept {
            const PollData& pollData =
                static_cast<void_ptr::VoidPtr*>(uvPollHandle->data)->cast<PollData>();
            FetchClient& fetchClient = pollData.fetchClient;
            const curl_socket_t socket = pollData.socket;
            try {
                unsigned int eventBitmask = 0;
                if (status < 0) {
                    eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_ERR);
                }
                if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_READABLE)) !=
                    0) {
                    eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_IN);
                }
                if ((static_cast<unsigned int>(events) & static_cast<unsigned int>(UV_WRITABLE)) !=
                    0) {
                    eventBitmask |= static_cast<unsigned int>(CURL_CSELECT_OUT);
                }
                const bool ok =
                    fetchClient.driveCurlTransfers(socket, static_cast<int>(eventBitmask));
                if (!ok) {
                    return;
                }
            } catch (...) {
                if (!fetchClient.pendingError.hasValue()) {
                    fetchClient.pendingError.exceptionPtr = std::current_exception();
                }
            }
        };

        unsigned int events = 0;
        if (what == CURL_POLL_IN) {
            // Call driveCurlTransfers() when this socket becomes readable.
            events = static_cast<unsigned int>(UV_READABLE);
        } else if (what == CURL_POLL_OUT) {
            // Call driveCurlTransfers() when this socket becomes writable.
            events = static_cast<unsigned int>(UV_WRITABLE);
        } else if (what == CURL_POLL_INOUT) {
            // Call driveCurlTransfers() when this socket becomes readable or
            // writable.
            events =
                static_cast<unsigned int>(UV_READABLE) | static_cast<unsigned int>(UV_WRITABLE);
        }
        const int status = uv_poll_start(&uvPollHandle, static_cast<int>(events), uvPollCallback);
        if (status != 0) {
            if (!fetchClient.pendingError.hasValue()) {
                fetchClient.pendingError = errors::code(status, uvErrorCategory);
            }
            return -1;
        }
        return 0;
    } catch (...) {
        if (!fetchClient.pendingError.hasValue()) {
            fetchClient.pendingError.exceptionPtr = std::current_exception();
        }
        return -1;
    }
}

// curl calls this function to update its timeout requirements.
//
// 1. timeout_ms < 0: Cancel the scheduled call to driveCurlTransfers().
//
// 2. timeout_ms >= 0: Schedule driveCurlTransfers() to run after this delay.
int FetchClient::curlMTimerFunction(CURLM* /* curlMultiHandle */, long timeout_ms,
    void* clientp) noexcept {
    const FetchClient& fetchClient =
        *static_cast<const void_ptr::VoidPtr*>(clientp)->cast<FetchClient*>();
    try {

        // Cancel the scheduled call to driveCurlTransfers().
        if (timeout_ms < 0) {
            const int status = uv_timer_stop(fetchClient.uvTimerHandle.get());
            if (status != 0) {
                if (!fetchClient.pendingError.hasValue()) {
                    fetchClient.pendingError = errors::code(status, uvErrorCategory);
                }
                return -1;
            }
            return 0;
        }

        // Refresh libuv's cached time so the delay starts from now.
        uv_update_time(&fetchClient.uvLoop);

        // libuv calls uvTimerCallback when the timer expires, which calls
        // driveCurlTransfers().
        const auto uvTimerCallback = [](uv_timer_t* uvTimerHandle) noexcept {
            FetchClient& fetchClient =
                *static_cast<void_ptr::VoidPtr*>(uvTimerHandle->data)->cast<FetchClient*>();
            try {
                const bool ok = fetchClient.driveCurlTransfers(CURL_SOCKET_TIMEOUT, 0);
                if (!ok) {
                    return;
                }
            } catch (...) {
                if (!fetchClient.pendingError.hasValue()) {
                    fetchClient.pendingError.exceptionPtr = std::current_exception();
                }
            }
        };

        // Schedule driveCurlTransfers() to run after this delay.
        const int status = uv_timer_start(fetchClient.uvTimerHandle.get(), uvTimerCallback,
            static_cast<uint64_t>(timeout_ms), 0);
        if (status != 0) {
            if (!fetchClient.pendingError.hasValue()) {
                fetchClient.pendingError = errors::code(status, uvErrorCategory);
            }
            return -1;
        }
        return 0;
    } catch (...) {
        if (!fetchClient.pendingError.hasValue()) {
            fetchClient.pendingError.exceptionPtr = std::current_exception();
        }
        return -1;
    }
}

// Settle pending promises with response readiness, body chunks, end-of-stream,
// or errors.
void FetchClient::uvAsyncCallback(uv_async_t* uvAsyncHandle) noexcept {
    FetchClient& fetchClient =
        *static_cast<void_ptr::VoidPtr*>(uvAsyncHandle->data)->cast<FetchClient*>();
    uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle));
    // Another callback may have recorded an error earlier in this iteration.
    if (fetchClient.pendingError.hasValue()) {
        return;
    }
    try {
        // Snapshot the fetchStateMap's CURL* keys + fetchStateID into an entries
        // list before looping over the entries. The fetchStateMap can be modified
        // during iteration, so we need a stable list to iterate over.
        std::vector<std::pair<CURL*, uint64_t>> entries;
        entries.reserve(fetchClient.fetchStateMap.size());
        for (const std::pair<CURL* const, FetchState>& entry : fetchClient.fetchStateMap) {
            entries.push_back({entry.first, entry.second.fetchStateID});
        }

        // Loop over the entries.
        for (const std::pair<CURL*, uint64_t>& entry : entries) {
            if (fetchClient.pendingError.hasValue()) {
                return;
            }
            CURL* curlEasyHandle = entry.first;
            const uint64_t fetchStateID = entry.second;

            // Earlier iterations run JS promise jobs, which may close other
            // responses and remove their states from the map. Skip handles whose
            // states have been removed since entries was snapshotted.
            if (!fetchClient.fetchStateMap.contains(curlEasyHandle)) {
                continue;
            }

            // Get the fetchState for the give curlEasyHandle.
            //
            // Curl may reuse CURL* pointers, so we need to additionally check if
            // the fetchStateID aligns.
            FetchState& fetchState = fetchClient.fetchStateMap.at(curlEasyHandle);
            if (fetchState.fetchStateID != fetchStateID) {
                continue;
            }

            // If fetchState was abandoned (the QuickJS GC finalizer sets this
            // flag), it means the user forgot to call fetchResponse.close() and is
            // no longer referencing the `fetchResponse` variable. Terminate its
            // current ready/read promises with AbortError.
            if (fetchState.abandoned) {
                const std::map<CURL*, FetchState>::node_type mapNode =
                    fetchClient.fetchStateMap.extract(curlEasyHandle);
                if (mapNode.empty()) {
                    std::terminate(); // An extracted abandoned state must exist.
                }
                FetchState& fetchState = mapNode.mapped();
                const errors::Error error = fetchState.close();
                if (error.hasValue()) {
                    if (!fetchClient.pendingError.hasValue()) {
                        fetchClient.pendingError = error;
                    }
                }
                if (fetchClient.pendingError.hasValue()) {
                    return;
                }
                // Retain settlement functions locally: close() releases the state's
                // JS references. These moves cannot throw, so close precedes all exits.
                owned::JSPromise responseReadyPromise = std::move(fetchState.responseReadyPromise);
                std::optional<owned::JSPromise> responseReadPromise =
                    std::move(fetchState.responseReadPromise);
                const JSPromiseStateEnum responseReadyPromiseState =
                    JS_PromiseState(&fetchClient.jsContext, responseReadyPromise.promise.get());
                if (responseReadyPromiseState != JS_PROMISE_PENDING &&
                    !responseReadPromise.has_value()) {
                    continue;
                }
                owned::JSValue jsError =
                    jsNewError(&fetchClient.jsContext, "AbortError", "FetchResponse is closed");
                // Reject responseReadyPromise.
                if (responseReadyPromiseState == JS_PROMISE_PENDING) {
                    const JSValue result = JS_Call(&fetchClient.jsContext,
                        responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
                    JS_FreeValue(&fetchClient.jsContext, result);
                }
                // Reject responseReadPromise.
                if (responseReadPromise.has_value()) {
                    const JSPromiseStateEnum responseReadPromiseState =
                        JS_PromiseState(&fetchClient.jsContext,
                            responseReadPromise.value().promise.get());
                    if (responseReadPromiseState == JS_PROMISE_PENDING) {
                        const JSValue result = JS_Call(&fetchClient.jsContext,
                            responseReadPromise.value().rejectFunction.get(), JS_UNDEFINED, 1,
                            &jsError.get());
                        JS_FreeValue(&fetchClient.jsContext, result);
                    }
                }
                fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                continue;
            }

            // If responseReadyPromise has not yet been settled, settle it first.
            const JSPromiseStateEnum responseReadyPromiseState =
                JS_PromiseState(&fetchClient.jsContext,
                    fetchState.responseReadyPromise.promise.get());
            if (responseReadyPromiseState != JS_PROMISE_FULFILLED &&
                responseReadyPromiseState != JS_PROMISE_REJECTED) {
                if (!fetchState.responseHeadersComplete) {
                    // The transfer has not yet completed, don't settle the
                    // responseReadyPromise.
                    if (fetchState.responseCurlTransferStatus != CurlTransferStatus::Completed) {
                        continue;
                    }

                    // The transfer completed without receiving any response
                    // headers. Reject responseReadyPromise.
                    owned::JSValue jsError = jsNewError(&fetchClient.jsContext, "TypeError",
                        fetchState.error.toString());
                    const JSValue result = JS_Call(&fetchClient.jsContext,
                        fetchState.responseReadyPromise.rejectFunction.get(), JS_UNDEFINED, 1,
                        &jsError.get());
                    JS_FreeValue(&fetchClient.jsContext, result);
                    fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                    continue;
                }

                // Resolve resonseReadyPromise.
                const JSValue result = JS_Call(&fetchClient.jsContext,
                    fetchState.responseReadyPromise.resolveFunction.get(), JS_UNDEFINED, 0,
                    nullptr);
                JS_FreeValue(&fetchClient.jsContext, result);
                fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                continue;
            }

            // No responseReadPromise needs settling.
            if (!fetchState.responseReadPromise.has_value()) {
                continue;
            }

            // No error, buffered data, or end-of-stream to settle the
            // responseReadPromise with.
            if (!fetchState.error.hasValue() && fetchState.responseBuffer.empty() &&
                fetchState.responseCurlTransferStatus != CurlTransferStatus::Completed) {
                continue;
            }

            // Move fetchState.responseReadPromise out to a local variable and
            // reset it so JS can request another read.
            owned::JSPromise responseReadPromise =
                std::move(fetchState.responseReadPromise.value());
            fetchState.responseReadPromise.reset();

            // Reject the responseReadPromise if the transfer encountered an error.
            if (fetchState.error.hasValue()) {
                owned::JSValue jsError =
                    jsNewError(&fetchClient.jsContext, "TypeError", fetchState.error.toString());
                const JSValue result = JS_Call(&fetchClient.jsContext,
                    responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &jsError.get());
                JS_FreeValue(&fetchClient.jsContext, result);
                fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                continue;
            }

            // No error or buffered chunks remain, and the transfer has completed.
            // Resolve the responseReadPromise with null to indicate end-of-stream.
            if (fetchState.responseBuffer.empty()) {
                JSValue jsNull = JS_NULL;
                const JSValue result = JS_Call(&fetchClient.jsContext,
                    responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &jsNull);
                JS_FreeValue(&fetchClient.jsContext, result);
                fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                continue;
            }

            // Pop the oldest chunk from fetchState.responseBuffer.
            const std::vector<uint8_t> chunk = std::move(fetchState.responseBuffer.front());
            fetchState.responseBuffer.pop_front();
            fetchState.responseBufferSize -= chunk.size();

            // If curl is currently paused and popping that chunk from the
            // responseBuffer dropped responseBufferSize to less than half of
            // bodyBufferLimit, unpause curl.
            if (fetchState.responseCurlTransferStatus == CurlTransferStatus::Paused &&
                fetchState.responseBufferSize < bodyBufferLimit / 2) {
                fetchState.responseCurlTransferStatus = CurlTransferStatus::Running;
                const CURLcode curlCode = curl_easy_pause(curlEasyHandle, CURLPAUSE_CONT);
                if (curlCode != CURLE_OK) {
                    fetchState.error = errors::message(curl_easy_strerror(curlCode));
                    fetchState.responseCurlTransferStatus = CurlTransferStatus::Completed;
                    if (fetchState.curlMultiHandle != nullptr) {
                        const CURLMcode curlMCode =
                            curl_multi_remove_handle(fetchState.curlMultiHandle, curlEasyHandle);
                        if (curlMCode != CURLM_OK) {
                            if (!fetchClient.pendingError.hasValue()) {
                                fetchClient.pendingError =
                                    errors::code(curlMCode, curlMErrorCategory);
                            }
                            return;
                        }
                        fetchState.curlMultiHandle = nullptr;
                    }
                }
                const bool ok = fetchClient.driveCurlTransfers(CURL_SOCKET_TIMEOUT, 0);
                if (!ok) {
                    return;
                }
            }

            // Convert the chunk into a JS Uint8Array.
            owned::JSValue uint8Array(&fetchClient.jsContext,
                JS_NewUint8ArrayCopy(&fetchClient.jsContext, chunk.data(), chunk.size()));
            if (JS_IsException(uint8Array.get())) {
                owned::JSValue error(&fetchClient.jsContext,
                    JS_GetException(&fetchClient.jsContext));
                const JSValue result = JS_Call(&fetchClient.jsContext,
                    responseReadPromise.rejectFunction.get(), JS_UNDEFINED, 1, &error.get());
                JS_FreeValue(&fetchClient.jsContext, result);
                fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
                continue;
            }

            // Resolve the responseReadPromise with the Uint8Array chunk.
            const JSValue result = JS_Call(&fetchClient.jsContext,
                responseReadPromise.resolveFunction.get(), JS_UNDEFINED, 1, &uint8Array.get());
            JS_FreeValue(&fetchClient.jsContext, result);
            fetchClient.jsExecutePendingJobs(fetchClient.jsContext);
        }
    } catch (...) {
        if (!fetchClient.pendingError.hasValue()) {
            fetchClient.pendingError.exceptionPtr = std::current_exception();
        }
    }
}

FetchResponse::FetchResponse(FetchClient& fetchClient, CURL* curlEasyHandle)
    : fetchClient(fetchClient) {
    this->curlEasyHandle = curlEasyHandle;
}

// Validate the receiver without requiring an open or ready response.
// The returned reference is borrowed; failures leave a pending JS exception.
std::optional<std::reference_wrapper<FetchResponse>>
FetchResponse::fromThisValue(JSContext& jsContext, JSValueConst thisValue) {
    JSClassID fetchResponseClassID = 0;
    {
        const JSRuntime& jsRuntime = *JS_GetRuntime(&jsContext);
        const std::lock_guard<std::mutex> lock(classIDMutex);
        if (!fetchResponseClassIDMap.contains(&jsRuntime)) {
            JS_ThrowInternalError(&jsContext, "FetchResponse class is not registered");
            return std::nullopt;
        }
        fetchResponseClassID = fetchResponseClassIDMap.at(&jsRuntime);
    }
    const void* opaque = JS_GetOpaque2(&jsContext, thisValue, fetchResponseClassID);
    if (opaque == nullptr) {
        return std::nullopt;
    }
    FetchResponse& fetchResponse =
        static_cast<const void_ptr::VoidPtr*>(opaque)->cast<FetchResponse>();
    if (this_thread::get_id() != fetchResponse.fetchClient.ownerThreadID) {
        std::terminate();
    }
    // JS can catch a native failure before control returns to the host.
    if (fetchResponse.fetchClient.pendingError.hasValue()) {
        JS_ThrowInternalError(&jsContext, "FetchResponse: native failure");
        return std::nullopt;
    }
    return fetchResponse;
}

JSValue FetchResponse::read(JSContext* jsContext, JSValueConst thisValue, int /* argc */,
    JSValueConst* /* argv */) {
    try {
        std::optional<std::reference_wrapper<FetchResponse>> fetchResponseResult =
            fromThisValue(*jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = fetchResponseResult.value().get();
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.read: response is closed");
        }
        if (!fetchResponse.fetchClient.fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext,
                "FetchResponse.read: response state is missing");
        }
        FetchState& fetchState =
            fetchResponse.fetchClient.fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState =
            JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED &&
            responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.read: response not ready");
        }
        if (fetchState.responseReadPromise.has_value()) {
            return JS_ThrowTypeError(jsContext,
                "FetchResponse.read: another read is already pending");
        }
        std::array resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        owned::JSValue promise(jsContext,
            JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(promise.get())) {
            return JS_EXCEPTION;
        }
        fetchState.responseReadPromise = owned::JSPromise{
            .promise = std::move(promise),
            .resolveFunction = owned::JSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = owned::JSValue(jsContext, resolvingFunctions.at(1)),
        };
        const bool ok = fetchResponse.fetchClient.triggerPromiseSettlement();
        if (!ok) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.read: native failure");
        }
        return JS_DupValue(jsContext, fetchState.responseReadPromise.value().promise.get());
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.read: native failure");
    }
}

JSValue FetchResponse::close(JSContext* jsContext, JSValueConst thisValue, int /* argc */,
    JSValueConst* /* argv */) {
    try {
        std::optional<std::reference_wrapper<FetchResponse>> fetchResponseResult =
            fromThisValue(*jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return JS_EXCEPTION;
        }
        FetchResponse& fetchResponse = fetchResponseResult.value().get();
        CURL* curlEasyHandle = fetchResponse.curlEasyHandle;
        fetchResponse.curlEasyHandle = nullptr;
        if (curlEasyHandle == nullptr) {
            return JS_UNDEFINED;
        }
        const std::map<CURL*, FetchState>::node_type mapNode =
            fetchResponse.fetchClient.fetchStateMap.extract(curlEasyHandle);
        if (mapNode.empty()) {
            return JS_UNDEFINED;
        }
        FetchState& fetchState = mapNode.mapped();
        // Move the responseReadyPromise and responseReadPromise out of the
        // fetchState before calling close() on it.
        owned::JSPromise responseReadyPromise = std::move(fetchState.responseReadyPromise);
        std::optional<owned::JSPromise> responseReadPromise =
            std::move(fetchState.responseReadPromise);
        const errors::Error error = fetchState.close();
        if (error.hasValue()) {
            if (!fetchResponse.fetchClient.pendingError.hasValue()) {
                fetchResponse.fetchClient.pendingError = error;
            }
        }
        if (fetchResponse.fetchClient.pendingError.hasValue()) {
            return JS_ThrowInternalError(jsContext, "FetchResponse.close: native failure");
        }
        const JSPromiseStateEnum responseReadyPromiseState =
            JS_PromiseState(jsContext, responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_PENDING && !responseReadPromise.has_value()) {
            return JS_UNDEFINED;
        }
        owned::JSValue jsError = jsNewError(jsContext, "AbortError", "FetchResponse is closed");
        if (responseReadyPromiseState == JS_PROMISE_PENDING) {
            const JSValue result = JS_Call(jsContext, responseReadyPromise.rejectFunction.get(),
                JS_UNDEFINED, 1, &jsError.get());
            JS_FreeValue(jsContext, result);
        }
        if (responseReadPromise.has_value()) {
            const JSPromiseStateEnum responseReadPromiseState =
                JS_PromiseState(jsContext, responseReadPromise.value().promise.get());
            if (responseReadPromiseState == JS_PROMISE_PENDING) {
                const JSValue result =
                    JS_Call(jsContext, responseReadPromise.value().rejectFunction.get(),
                        JS_UNDEFINED, 1, &jsError.get());
                JS_FreeValue(jsContext, result);
            }
        }
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.close: native failure");
    }
}

JSValue FetchResponse::getStatus(JSContext* jsContext, JSValueConst thisValue, int /* argc */,
    JSValueConst* /* argv */) {
    try {
        std::optional<std::reference_wrapper<FetchResponse>> fetchResponseResult =
            fromThisValue(*jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = fetchResponseResult.value().get();
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getStatus: response is closed");
        }
        if (!fetchResponse.fetchClient.fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext,
                "FetchResponse.getStatus: response state is missing");
        }
        const FetchState& fetchState =
            fetchResponse.fetchClient.fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState =
            JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED &&
            responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getStatus: response not ready");
        }
        return JS_NewInt64(jsContext, fetchState.responseCode);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getStatus: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getStatus: native failure");
    }
}

JSValue FetchResponse::getHeaders(JSContext* jsContext, JSValueConst thisValue, int /* argc */,
    JSValueConst* /* argv */) {
    try {
        std::optional<std::reference_wrapper<FetchResponse>> fetchResponseResult =
            fromThisValue(*jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = fetchResponseResult.value().get();
        const FetchClient& fetchClient = fetchResponse.fetchClient;
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getHeaders: response is closed");
        }
        if (!fetchClient.fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext,
                "FetchResponse.getHeaders: response state is missing");
        }
        const FetchState& fetchState = fetchClient.fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState =
            JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED &&
            responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getHeaders: response not ready");
        }
        std::vector<JSONValue> headers;
        headers.reserve(fetchState.responseHeaders.size());
        for (const std::pair<std::string, std::string>& header : fetchState.responseHeaders) {
            headers.push_back(JSONValue::array({
                JSONValue::string(header.first),
                JSONValue::string(header.second),
            }));
        }
        return JSONValue::array(headers).toJSValue(jsContext);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getHeaders: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getHeaders: native failure");
    }
}

JSValue FetchResponse::getURL(JSContext* jsContext, JSValueConst thisValue, int /* argc */,
    JSValueConst* /* argv */) {
    try {
        std::optional<std::reference_wrapper<FetchResponse>> fetchResponseResult =
            fromThisValue(*jsContext, thisValue);
        if (!fetchResponseResult.has_value()) {
            return JS_EXCEPTION;
        }
        const FetchResponse& fetchResponse = fetchResponseResult.value().get();
        const FetchClient& fetchClient = fetchResponse.fetchClient;
        if (fetchResponse.curlEasyHandle == nullptr) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getURL: response is closed");
        }
        if (!fetchClient.fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
            return JS_ThrowInternalError(jsContext,
                "FetchResponse.getURL: response state is missing");
        }
        const FetchState& fetchState = fetchClient.fetchStateMap.at(fetchResponse.curlEasyHandle);
        const JSPromiseStateEnum responseReadyPromiseState =
            JS_PromiseState(jsContext, fetchState.responseReadyPromise.promise.get());
        if (responseReadyPromiseState != JS_PROMISE_FULFILLED &&
            responseReadyPromiseState != JS_PROMISE_REJECTED) {
            return JS_ThrowTypeError(jsContext, "FetchResponse.getURL: response not ready");
        }
        return JS_NewStringLen(jsContext, fetchState.effectiveURL.data(),
            fetchState.effectiveURL.size());
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getURL: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "FetchResponse.getURL: native failure");
    }
}

JSValue FetchClient::send(JSContext* jsContext, JSValueConst thisValue, int argc,
    JSValueConst* argv) {
    FetchClient* fetchClient = nullptr;
    CURL* curlEasyHandle = nullptr;
    try {
        JSClassID fetchClientClassID = 0;
        {
            const JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(classIDMutex);
            if (!fetchClientClassIDMap.contains(jsRuntime)) {
                return JS_ThrowInternalError(jsContext, "FetchClient class is not registered");
            }
            fetchClientClassID = fetchClientClassIDMap.at(jsRuntime);
        }
        const void* opaque = JS_GetOpaque2(jsContext, thisValue, fetchClientClassID);
        if (opaque == nullptr) {
            return JS_EXCEPTION;
        }
        fetchClient = static_cast<const void_ptr::VoidPtr*>(opaque)->cast<FetchClient*>();
        if (this_thread::get_id() != fetchClient->ownerThreadID) {
            std::terminate();
        }
        if (fetchClient->pendingError.hasValue()) {
            return JS_ThrowInternalError(jsContext, "FetchClient.send: native failure");
        }
        JSClassID fetchResponseClassID = 0;
        {
            const JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
            const std::lock_guard<std::mutex> lock(classIDMutex);
            if (!fetchResponseClassIDMap.contains(jsRuntime)) {
                return JS_ThrowInternalError(jsContext, "FetchResponse class is not registered");
            }
            fetchResponseClassID = fetchResponseClassIDMap.at(jsRuntime);
        }
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext,
                "FetchClient.send: requires at least 1 argument (%d provided)", argc);
        }
        std::optional<FetchRequest> fetchRequest = FetchRequest::create(*jsContext, argv[0]);
        if (!fetchRequest.has_value()) {
            return JS_EXCEPTION;
        }
        if (fetchClient->fetchStateMap.size() >= 64) {
            return JS_ThrowRangeError(jsContext, "Too many open FetchResponses");
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> ownedCurlEasyHandle(curl_easy_init(),
            curl_easy_cleanup);
        if (ownedCurlEasyHandle == nullptr) {
            return JS_ThrowOutOfMemory(jsContext);
        }
        fetchClient->fetchStateMap.try_emplace(ownedCurlEasyHandle.get());
        FetchState& fetchState = fetchClient->fetchStateMap.at(ownedCurlEasyHandle.get());
        fetchState.curlVoidPtr.assign(&fetchState);
        fetchClient->fetchStateIDCounter++;
        fetchState.fetchStateID = fetchClient->fetchStateIDCounter;
        fetchState.request = fetchRequest.value();
        curlEasyHandle = ownedCurlEasyHandle.get();
        fetchState.ownedCurlEasyHandle = ownedCurlEasyHandle.release();
        bool hasContentType = false;
        for (const std::pair<std::string, std::string>& header : fetchState.request.headers) {
            std::string lowerName = header.first;
            for (char& character : lowerName) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (lowerName == "content-type") {
                hasContentType = true;
            }
            std::string line = header.first + ": " + header.second;
            if (header.second.empty()) {
                line = header.first + ";"; // Curl's syntax for an explicitly empty header.
            }
            curl_slist* curlHeaders =
                curl_slist_append(fetchState.ownedCurlRequestHeaders, line.c_str());
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.ownedCurlRequestHeaders = curlHeaders;
        }
        if (!hasContentType) {
            // POSTFIELDS otherwise invents application/x-www-form-urlencoded.
            // The JS facade supplies a content type for strings, but not bytes.
            curl_slist* curlHeaders =
                curl_slist_append(fetchState.ownedCurlRequestHeaders, "Content-Type:");
            if (curlHeaders == nullptr) {
                throw std::bad_alloc();
            }
            fetchState.ownedCurlRequestHeaders = curlHeaders;
        }
        CURLcode curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_URL,
            fetchState.request.url.c_str());
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode =
            curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_PROTOCOLS_STR, "http,https");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_REDIR_PROTOCOLS_STR,
            "http,https");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode =
            curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_FOLLOWLOCATION,
            CURLFOLLOW_OBEYCODE);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_MAXREDIRS, 20L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode =
            curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOSIGNAL, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYPEER, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SSL_VERIFYHOST, 2L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode =
            curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_ACCEPT_ENCODING, "");
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HTTPHEADER,
            fetchState.ownedCurlRequestHeaders);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERDATA,
            &fetchState.curlVoidPtr);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_HEADERFUNCTION,
            FetchState::curlHeaderFunction);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEDATA,
            &fetchState.curlVoidPtr);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        curlCode = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_WRITEFUNCTION,
            FetchState::curlWriteFunction);
        if (curlCode != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(curlCode));
        }
        CURLcode methodStatus = CURLE_OK;
        if (fetchState.request.method == "HEAD") {
            methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_NOBODY, 1L);
        } else if (fetchState.request.method != "GET") {
            const char* bodyBytes = "";
            if (!fetchState.request.body.empty()) {
                bodyBytes = reinterpret_cast<const char*>(fetchState.request.body.data());
            }
            methodStatus =
                curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDS, bodyBytes);
            if (methodStatus == CURLE_OK) {
                methodStatus =
                    curl_easy_setopt(fetchState.ownedCurlEasyHandle, CURLOPT_POSTFIELDSIZE_LARGE,
                        static_cast<curl_off_t>(fetchState.request.body.size()));
            }
            if (methodStatus == CURLE_OK && fetchState.request.method != "POST") {
                methodStatus = curl_easy_setopt(fetchState.ownedCurlEasyHandle,
                    CURLOPT_CUSTOMREQUEST, fetchState.request.method.c_str());
            }
        }
        if (methodStatus != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(methodStatus));
        }
        std::array<JSValue, 2> resolvingFunctions = {JS_UNDEFINED, JS_UNDEFINED};
        owned::JSValue ready(jsContext,
            JS_NewPromiseCapability(jsContext, resolvingFunctions.data()));
        if (JS_IsException(ready.get())) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        fetchState.responseReadyPromise = owned::JSPromise{
            .promise = std::move(ready),
            .resolveFunction = owned::JSValue(jsContext, resolvingFunctions.at(0)),
            .rejectFunction = owned::JSValue(jsContext, resolvingFunctions.at(1)),
        };
        owned::JSValue response(jsContext, JS_NewObjectClass(jsContext, fetchResponseClassID));
        if (JS_IsException(response.get())) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        void_ptr::VoidPtr* voidPtr =
            new void_ptr::VoidPtr(FetchResponse(*fetchClient, curlEasyHandle));
        if (JS_SetOpaque(response.get(), voidPtr) < 0) {
            delete voidPtr;
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_ThrowInternalError(jsContext, "Could not attach FetchResponse");
        }
        if (JS_DefinePropertyValueStr(jsContext, response.get(), "ready",
                JS_DupValue(jsContext, fetchState.responseReadyPromise.promise.get()),
                JS_PROP_ENUMERABLE) < 0) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_EXCEPTION;
        }
        const CURLMcode curlMCode = curl_multi_add_handle(fetchClient->ownedCurlMultiHandle,
            fetchState.ownedCurlEasyHandle);
        if (curlMCode != CURLM_OK) {
            const errors::Error closeError = fetchState.close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
            return JS_ThrowInternalError(jsContext, "curl_multi_add_handle: %s",
                curl_multi_strerror(curlMCode));
        }
        fetchState.curlMultiHandle = fetchClient->ownedCurlMultiHandle;
        fetchState.responseCurlTransferStatus = CurlTransferStatus::Running;
        return response.release();
    } catch (const std::bad_alloc&) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: %s", exception.what());
    } catch (...) {
        if (curlEasyHandle != nullptr && fetchClient->fetchStateMap.contains(curlEasyHandle)) {
            const errors::Error closeError = fetchClient->fetchStateMap.at(curlEasyHandle).close();
            if (!fetchClient->pendingError.hasValue()) {
                if (closeError.hasValue()) {
                    fetchClient->pendingError = closeError;
                }
            }
            fetchClient->fetchStateMap.erase(curlEasyHandle);
        }
        return JS_ThrowInternalError(jsContext, "FetchClient.send: unexpected native failure");
    }
}

FetchState::~FetchState() {
    std::ignore = close();
}

errors::Error FetchState::close() noexcept {
    errors::Error error;
    if (curlMultiHandle != nullptr) {
        const CURLMcode curlMCode = curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle);
        if (curlMCode != CURLM_OK) {
            if (curlMCode != CURLM_ABORTED_BY_CALLBACK && curlMCode != CURLM_OUT_OF_MEMORY) {
                std::terminate();
            }
            error = errors::code(curlMCode, curlMErrorCategory);
        }
        curlMultiHandle = nullptr;
    }
    if (ownedCurlEasyHandle != nullptr) {
        curl_easy_cleanup(ownedCurlEasyHandle);
        ownedCurlEasyHandle = nullptr;
    }
    curl_slist_free_all(ownedCurlRequestHeaders);
    ownedCurlRequestHeaders = nullptr;
    responseReadyPromise = {};
    responseReadPromise = std::nullopt;
    return error;
}

FetchClient::FetchClient(JSContext& jsContext, uv_loop_t& uvLoop, errors::Error& pendingError)
    : pendingError(pendingError), jsContext(jsContext), uvLoop(uvLoop) {}

std::expected<std::unique_ptr<FetchClient>, errors::Error> FetchClient::create(JSContext& jsContext,
    uv_loop_t& uvLoop, errors::Error& pendingError,
    std::function<void(JSContext&)> jsExecutePendingJobs) {
    if (jsExecutePendingJobs == nullptr) {
        return std::unexpected(errors::message("jsExecutePendingJobs is nullptr"));
    }

    std::unique_ptr<FetchClient> fetchClient(new FetchClient(jsContext, uvLoop, pendingError));
    {
        owned::UvHandle<uv_async_t> uvAsyncHandle(std::make_unique<uv_async_t>());
        const int status =
            uv_async_init(&uvLoop, uvAsyncHandle.get(), FetchClient::uvAsyncCallback);
        if (status != 0) {
            return std::unexpected(errors::code(status, uvErrorCategory));
        }
        uvAsyncHandle.setInitialized();
        uvAsyncHandle.get()->data = new void_ptr::VoidPtr(fetchClient.get());
        uv_unref(reinterpret_cast<uv_handle_t*>(uvAsyncHandle.get()));
        fetchClient->uvAsyncHandle = std::move(uvAsyncHandle);
    }
    fetchClient->jsExecutePendingJobs = std::move(jsExecutePendingJobs);
    fetchClient->curlVoidPtr = std::make_unique<void_ptr::VoidPtr>(fetchClient.get());
    fetchClient->ownedCurlMultiHandle = curl_multi_init();
    if (fetchClient->ownedCurlMultiHandle == nullptr) {
        return std::unexpected(errors::code(std::errc::not_enough_memory, std::generic_category()));
    }
    {
        owned::UvHandle<uv_timer_t> uvTimerHandle(std::make_unique<uv_timer_t>());
        const int status = uv_timer_init(&uvLoop, uvTimerHandle.get());
        if (status != 0) {
            return std::unexpected(errors::code(status, uvErrorCategory));
        }
        uvTimerHandle.setInitialized();
        uvTimerHandle.get()->data = new void_ptr::VoidPtr(fetchClient.get());
        fetchClient->uvTimerHandle = std::move(uvTimerHandle);
    }

    CURLMcode curlMCode = curl_multi_setopt(fetchClient->ownedCurlMultiHandle, CURLMOPT_SOCKETDATA,
        fetchClient->curlVoidPtr.get());
    if (curlMCode != CURLM_OK) {
        return std::unexpected(errors::code(curlMCode, curlMErrorCategory));
    }
    curlMCode = curl_multi_setopt(fetchClient->ownedCurlMultiHandle, CURLMOPT_SOCKETFUNCTION,
        FetchClient::curlMSocketFunction);
    if (curlMCode != CURLM_OK) {
        return std::unexpected(errors::code(curlMCode, curlMErrorCategory));
    }
    curlMCode = curl_multi_setopt(fetchClient->ownedCurlMultiHandle, CURLMOPT_TIMERDATA,
        fetchClient->curlVoidPtr.get());
    if (curlMCode != CURLM_OK) {
        return std::unexpected(errors::code(curlMCode, curlMErrorCategory));
    }
    curlMCode = curl_multi_setopt(fetchClient->ownedCurlMultiHandle, CURLMOPT_TIMERFUNCTION,
        FetchClient::curlMTimerFunction);
    if (curlMCode != CURLM_OK) {
        return std::unexpected(errors::code(curlMCode, curlMErrorCategory));
    }
    return fetchClient;
}

FetchClient::~FetchClient() {
    std::ignore = close();
}

errors::Error FetchClient::close() noexcept {
    if (this_thread::get_id() != ownerThreadID) {
        std::terminate();
    }
    if (closed) {
        return {};
    }
    closed = true;
    errors::Error firstError = pendingError;
    for (std::pair<CURL* const, FetchState>& entry : fetchStateMap) {
        const errors::Error error = entry.second.close();
        if (!firstError.hasValue()) {
            firstError = pendingError;
            if (!firstError.hasValue()) {
                firstError = error;
            }
        }
    }
    fetchStateMap.clear();
    if (ownedCurlMultiHandle != nullptr) {
        const CURLMcode curlMCode = curl_multi_cleanup(ownedCurlMultiHandle);
        if (curlMCode != CURLM_OK) {
            std::terminate();
        }
        ownedCurlMultiHandle = nullptr;
    }
    if (!firstError.hasValue()) {
        firstError = pendingError;
    }
    // Multi cleanup can invoke socket callbacks; release their dependencies last.
    curlVoidPtr.reset();
    uvPollHandleMap.clear();
    uvTimerHandle.reset();
    uvAsyncHandle.reset();
    return firstError;
}

JSModuleDef* createFetchImplModule(JSContext& jsContext, FetchClient& fetchClient) {
    try {
        if (this_thread::get_id() != fetchClient.ownerThreadID) {
            std::terminate();
        }
        JSRuntime& jsRuntime = *JS_GetRuntime(&jsContext);
        JSClassID clientClassID = 0;
        {
            const std::lock_guard<std::mutex> lock(classIDMutex);
            if (fetchClientClassIDMap.contains(&jsRuntime)) {
                clientClassID = fetchClientClassIDMap.at(&jsRuntime);
            } else {
                JS_NewClassID(&jsRuntime, &clientClassID);
                fetchClientClassIDMap.insert_or_assign(&jsRuntime, clientClassID);
                const auto jsRuntimeFinalizer = [](JSRuntime* jsRuntime, void* /* argument */) {
                    const std::lock_guard<std::mutex> lock(classIDMutex);
                    fetchClientClassIDMap.erase(jsRuntime);
                };
                const int status = JS_AddRuntimeFinalizer(&jsRuntime, jsRuntimeFinalizer, nullptr);
                if (status < 0) {
                    fetchClientClassIDMap.erase(&jsRuntime);
                    throw std::bad_alloc();
                }
            }
        }
        const JSClassDef clientClass = {
            .class_name = "FetchClient",
            .finalizer =
                [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                    delete static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value,
                        JS_GetClassID(value)));
                },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (!JS_IsRegisteredClass(&jsRuntime, clientClassID) &&
            JS_NewClass(&jsRuntime, clientClassID, &clientClass) < 0) {
            JS_ThrowOutOfMemory(&jsContext);
            return nullptr;
        }
        JSClassID fetchResponseClassID = 0;
        {
            const std::lock_guard<std::mutex> lock(classIDMutex);
            if (fetchResponseClassIDMap.contains(&jsRuntime)) {
                fetchResponseClassID = fetchResponseClassIDMap.at(&jsRuntime);
            } else {
                JS_NewClassID(&jsRuntime, &fetchResponseClassID);
                fetchResponseClassIDMap.insert_or_assign(&jsRuntime, fetchResponseClassID);
                const auto jsRuntimeFinalizer = [](JSRuntime* jsRuntime, void* /* argument */) {
                    const std::lock_guard<std::mutex> lock(classIDMutex);
                    fetchResponseClassIDMap.erase(jsRuntime);
                };
                const int status = JS_AddRuntimeFinalizer(&jsRuntime, jsRuntimeFinalizer, nullptr);
                if (status < 0) {
                    fetchResponseClassIDMap.erase(&jsRuntime);
                    throw std::bad_alloc();
                }
            }
        }
        const JSClassDef responseClass = {
            .class_name = "FetchResponse",
            .finalizer =
                [](JSRuntime* /* jsRuntime */, JSValue value) noexcept {
                    FetchClient* fetchClient = nullptr;
                    try {
                        const std::unique_ptr<void_ptr::VoidPtr> voidPtr(
                            static_cast<void_ptr::VoidPtr*>(JS_GetOpaque(value,
                                JS_GetClassID(value)))
                        );
                        if (voidPtr == nullptr) {
                            return;
                        }
                        const FetchResponse& fetchResponse = voidPtr->cast<FetchResponse>();
                        if (fetchResponse.curlEasyHandle == nullptr) {
                            return;
                        }
                        fetchClient = &fetchResponse.fetchClient;
                        // Context destruction can run this finalizer after client
                        // close(). The cleared map proves no work remains; do not
                        // touch the loop, async handle, or mailbox in that case.
                        if (!fetchClient->fetchStateMap.contains(fetchResponse.curlEasyHandle)) {
                            return;
                        }
                        // An earlier failure already requires host cleanup; do not
                        // schedule another settlement pass from this finalizer.
                        if (fetchClient->pendingError.hasValue()) {
                            return;
                        }
                        // GC can run while allocating promise results. Do
                        // not erase a FetchState currently being accessed
                        // by uvAsyncCallback(), and do not settle promises
                        // from inside a QuickJS finalizer.
                        fetchClient->fetchStateMap.at(fetchResponse.curlEasyHandle).abandoned =
                            true;
                        std::ignore = fetchClient->triggerPromiseSettlement();
                    } catch (...) {
                        if (fetchClient == nullptr) {
                            std::terminate(); // No reporter means invalid callback storage.
                        }
                        if (!fetchClient->pendingError.hasValue()) {
                            fetchClient->pendingError.exceptionPtr = std::current_exception();
                        }
                    }
                },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (!JS_IsRegisteredClass(&jsRuntime, fetchResponseClassID) &&
            JS_NewClass(&jsRuntime, fetchResponseClassID, &responseClass) < 0) {
            JS_ThrowOutOfMemory(&jsContext);
            return nullptr;
        }
        owned::JSValue client(&jsContext, JS_NewObjectClass(&jsContext, clientClassID));
        if (JS_IsException(client.get())) {
            return nullptr;
        }
        void_ptr::VoidPtr* voidPtr = new void_ptr::VoidPtr(&fetchClient);
        if (JS_SetOpaque(client.get(), voidPtr) < 0) {
            delete voidPtr;
            JS_ThrowInternalError(&jsContext, "Could not attach FetchClient");
            return nullptr;
        }
        const JSValue send = JS_NewCFunction(&jsContext, FetchClient::send, "send", 1);
        if (JS_IsException(send) || JS_DefinePropertyValueStr(&jsContext, client.get(), "send",
                                        send, JS_PROP_ENUMERABLE) < 0) {
            return nullptr;
        }
        owned::JSValue prototype(&jsContext, JS_NewObject(&jsContext));
        if (JS_IsException(prototype.get())) {
            return nullptr;
        }
        const std::array<std::pair<const char*, JSCFunction*>, 2> methods = {{
            {"read", FetchResponse::read},
            {"close", FetchResponse::close},
        }};
        for (const std::pair<const char*, JSCFunction*>& method : methods) {
            const JSValue function = JS_NewCFunction(&jsContext, method.second, method.first, 0);
            if (JS_IsException(function) || JS_DefinePropertyValueStr(&jsContext, prototype.get(),
                                                method.first, function, 0) < 0) {
                return nullptr;
            }
        }
        const std::array<std::pair<const char*, JSCFunction*>, 3> getters = {{
            {"status", FetchResponse::getStatus},
            {"headers", FetchResponse::getHeaders},
            {"url", FetchResponse::getURL},
        }};
        for (const std::pair<const char*, JSCFunction*>& getter : getters) {
            const JSValue function = JS_NewCFunction(&jsContext, getter.second, getter.first, 0);
            if (JS_IsException(function)) {
                return nullptr;
            }
            const JSAtom atom = JS_NewAtom(&jsContext, getter.first);
            const int status = JS_DefinePropertyGetSet(&jsContext, prototype.get(), atom, function,
                JS_UNDEFINED, JS_PROP_ENUMERABLE);
            JS_FreeAtom(&jsContext, atom);
            if (status < 0) {
                return nullptr;
            }
        }
        JS_SetClassProto(&jsContext, fetchResponseClassID, prototype.release());
        JSModuleDef* jsModuleDef = JS_NewCModule(&jsContext, "amj:fetch_impl",
            [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
                const JSValue client = JS_GetModulePrivateValue(jsContext, jsModuleDef);
                if (JS_IsException(client)) {
                    return -1;
                }
                return JS_SetModuleExport(jsContext, jsModuleDef, "fetchClient", client);
            });
        if (jsModuleDef == nullptr ||
            JS_AddModuleExport(&jsContext, jsModuleDef, "fetchClient") < 0 ||
            JS_SetModulePrivateValue(&jsContext, jsModuleDef, client.release()) < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(&jsContext);
        return nullptr;
    } catch (const std::exception& exception) {
        JS_ThrowInternalError(&jsContext, "createFetchImplModule: %s", exception.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(&jsContext, "createFetchImplModule: unexpected native failure");
        return nullptr;
    }
}

} // namespace amj::javascript
