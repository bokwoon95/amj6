#include "fetch_impl.hpp"

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "scheduler.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <httplib.h>
#include <quickjs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization): JS_MKVAL only constructs scalars.
const JSValue undefinedValue = JS_UNDEFINED;
const JSValue nullValue = JS_NULL;
const JSValue exceptionValue = JS_EXCEPTION;
// NOLINTEND(bugprone-throwing-static-initialization)
constexpr std::size_t headerLimit = 65536;
constexpr std::size_t bodyLimit = 33554432;
constexpr std::size_t chunkLimit = 65536;
constexpr std::size_t bufferLimit = 262144;

// Only this native data is accessed by the worker. The owner retains it until
// stop() joins the worker. Scheduler notifications happen outside this mutex.
struct Transport {
    Scheduler* scheduler = nullptr;
    std::unique_ptr<httplib::Client> client = nullptr;
    std::mutex mutex = {};
    std::condition_variable condition = {};
    std::deque<std::vector<unsigned char>> chunks = {};
    std::size_t bufferedBytes = 0;
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers = {};
    std::optional<FetchFailure> failure = {};
    std::exception_ptr workerException = nullptr;
    bool headersReceived = false;
    bool finished = false;
    bool canceled = false;
    int64_t headersID = 0; // Immutable after starting the worker.
    int64_t readID = 0;    // Protected by mutex.
    std::jthread worker = {};

    bool receiveHeaders(const httplib::Response& response) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (canceled || failure.has_value()) {
                return false;
            }
            if (headersReceived) {
                return true;
            }
            if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
                failure = FetchFailure{
                    .type = "RedirectRejected",
                    .message = "HTTP redirect rejected",
                };
                return false;
            }
            std::size_t bytes = 0;
            for (const httplib::Headers::value_type& header : response.headers) {
                for (const std::string* field : {&header.first, &header.second}) {
                    if (field->size() > headerLimit - bytes) {
                        failure = FetchFailure{
                            .type = "LimitExceeded",
                            .message = "HTTP response headers exceed 64 KiB",
                        };
                        return false;
                    }
                    bytes += field->size();
                }
                headers.push_back({header.first, header.second});
            }
            status = response.status;
            headersReceived = true;
        }
        const errors::Error error = scheduler->markReady(headersID);
        if (error.errorCode) {
            throw std::runtime_error(error.toString());
        }
        return true;
    }

    bool receiveBody(const char* data, std::size_t length) {
        std::size_t offset = 0;
        while (offset < length) {
            const std::size_t count = std::min(length - offset, chunkLimit);
            int64_t callbackID = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this, count]() {
                    return canceled || bufferedBytes <= bufferLimit - count;
                });
                if (canceled) {
                    return false;
                }
                // httplib provides length accessible bytes. offset/count are
                // bounded by length; no buffer points into JS-owned memory.
                chunks.emplace_back(data + offset, data + offset + count);
                bufferedBytes += count;
                callbackID = readID;
            }
            if (callbackID != 0) {
                const errors::Error error = scheduler->markReady(callbackID);
                if (error.errorCode) {
                    throw std::runtime_error(error.toString());
                }
            }
            offset += count;
        }
        return true;
    }

    void start(httplib::Request request) {
        request.response_handler = [this](const httplib::Response& response) {
            return receiveHeaders(response);
        };
        request.content_receiver = [this](const char* data, std::size_t length, std::size_t, std::size_t) {
            return receiveBody(data, length);
        };
        // NOLINTNEXTLINE(bugprone-exception-escape): Native failures are retained; failed final notification explicitly terminates.
        worker = std::jthread([this, request]() mutable {
            try {
                bool stopped = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    stopped = canceled;
                }
                if (!stopped) {
                    const httplib::Result result = client->send(request);
                    if (result) {
                        receiveHeaders(result.value());
                    } else {
                        const std::lock_guard<std::mutex> lock(mutex);
                        if (!failure.has_value() && !canceled) {
                            failure = FetchFailure{
                                .type = "TransportError",
                                .message = "HTTP transport failed: " + httplib::to_string(result.error()),
                            };
                        }
                    }
                }
            } catch (...) {
                const std::lock_guard<std::mutex> lock(mutex);
                workerException = std::current_exception();
            }
            int64_t callbackID = 0;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                finished = true;
                callbackID = readID;
            }
            condition.notify_all();
            try {
                const errors::Error headersError = scheduler->markReady(headersID);
                if (headersError.errorCode) {
                    std::terminate();
                }
                if (callbackID != 0) {
                    const errors::Error readError = scheduler->markReady(callbackID);
                    if (readError.errorCode) {
                        std::terminate();
                    }
                }
            } catch (...) {
                std::terminate(); // Do not leave the owner waiting forever.
            }
        });
    }

    void stop() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            canceled = true;
            chunks.clear();
            bufferedBytes = 0;
        }
        condition.notify_all();
        // stop() is not sticky: repeat to cover send()/connect starting after it.
        while (worker.joinable()) {
            client->stop();
            std::unique_lock<std::mutex> lock(mutex);
            if (finished) {
                break;
            }
            condition.wait_for(lock, chrono::milliseconds(10));
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    ~Transport() {
        try {
            stop();
        } catch (...) {
            std::terminate();
        }
    }
};

} // namespace

struct FetchRequestState {
    JSContext* jsContext = nullptr;
    int64_t requestID = 0;
    int64_t headersID = 0;
    int64_t readID = 0;
    std::unique_ptr<Transport> transport = nullptr;
    JSValue onHeaders = undefinedValue;
    JSValue onRead = undefinedValue;
    bool headersDelivered = false;
    bool closed = false;
    ~FetchRequestState() {
        JS_FreeValue(jsContext, onHeaders);
        JS_FreeValue(jsContext, onRead);
    }
};

struct FetchImplState {
    JSContext* jsContext = nullptr;
    Scheduler* scheduler = nullptr;
    std::map<int64_t, std::shared_ptr<FetchRequestState>> requests = {};
    int64_t lastRequestID = 0;
    bool active = false;
};

namespace {

struct FetchHandle {
    std::unique_ptr<FetchImpl> fetchImpl = nullptr;
    std::weak_ptr<FetchImplState> owner = {};
    std::weak_ptr<FetchRequestState> fetchRequestState = {};
};
std::mutex classIDMutex = {};
std::map<JSRuntime*, JSClassID> classIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

void releaseClassID(JSRuntime* jsRuntime, void* /* argument */) {
    const std::lock_guard<std::mutex> lock(classIDMutex);
    classIDMap.erase(jsRuntime);
}

JSClassID getClassID(JSRuntime* jsRuntime) {
    const std::lock_guard<std::mutex> lock(classIDMutex);
    if (classIDMap.contains(jsRuntime)) {
        return classIDMap.at(jsRuntime);
    }
    JSClassID classID = 0;
    JS_NewClassID(jsRuntime, &classID);
    classIDMap.insert_or_assign(jsRuntime, classID);
    if (JS_AddRuntimeFinalizer(jsRuntime, releaseClassID, nullptr) < 0) {
        classIDMap.erase(jsRuntime);
        throw std::bad_alloc();
    }
    return classID;
}

void finalizeHandle(JSRuntime* /* jsRuntime */, JSValue value) {
    const std::unique_ptr<FetchHandle> fetchHandle(static_cast<FetchHandle*>(JS_GetOpaque(value, JS_GetClassID(value))));
}

std::shared_ptr<FetchRequestState> getRequest(JSContext* jsContext, JSValueConst handle, const std::shared_ptr<FetchImplState>& fetchImplState) {
    const FetchHandle* fetchHandle = static_cast<FetchHandle*>(JS_GetOpaque2(jsContext, handle, getClassID(JS_GetRuntime(jsContext))));
    if (fetchHandle == nullptr) {
        return nullptr;
    }
    if (fetchHandle->fetchImpl != nullptr || fetchHandle->owner.lock() != fetchImplState) {
        JS_ThrowTypeError(jsContext, "Request belongs to a different native Fetch instance");
        return nullptr;
    }
    return fetchHandle->fetchRequestState.lock();
}

void closeRequest(const std::shared_ptr<FetchRequestState>& fetchRequestState, const std::shared_ptr<FetchImplState>& fetchImplState) {
    if (fetchRequestState->closed) {
        return;
    }
    fetchRequestState->closed = true;
    const errors::Error headersError = fetchImplState->scheduler->unregisterCallback(fetchRequestState->headersID);
    const errors::Error readError = fetchImplState->scheduler->unregisterCallback(fetchRequestState->readID);
    if (headersError.errorCode || readError.errorCode) {
        throw std::runtime_error("Could not unregister HTTP callbacks");
    }
    fetchRequestState->transport->stop();
    JS_FreeValue(fetchRequestState->jsContext, std::exchange(fetchRequestState->onHeaders, undefinedValue));
    JS_FreeValue(fetchRequestState->jsContext, std::exchange(fetchRequestState->onRead, undefinedValue));
    fetchImplState->requests.erase(fetchRequestState->requestID);
}

// Converts native completion data only. JS creates Errors and settles Promises.
errors::Error deliver(const std::shared_ptr<FetchRequestState>& fetchRequestState, bool reading) {
    if (fetchRequestState->closed) {
        return {};
    }
    std::optional<FetchFailure> failure;
    FetchResponseMetadata metadata;
    std::vector<unsigned char> chunk;
    bool done = false;
    {
        const std::lock_guard<std::mutex> lock(fetchRequestState->transport->mutex);
        if (fetchRequestState->transport->workerException != nullptr) {
            std::rethrow_exception(fetchRequestState->transport->workerException);
        }
        if (reading) {
            fetchRequestState->transport->readID = 0;
            if (!fetchRequestState->transport->chunks.empty()) {
                chunk = fetchRequestState->transport->chunks.front();
                fetchRequestState->transport->chunks.pop_front();
                fetchRequestState->transport->bufferedBytes -= chunk.size();
            } else if (fetchRequestState->transport->finished) {
                failure = fetchRequestState->transport->failure;
                done = true;
            } else {
                throw std::logic_error("HTTP read notified without data or completion");
            }
        } else if (fetchRequestState->transport->headersReceived) {
            metadata.status = fetchRequestState->transport->status;
            metadata.headers = fetchRequestState->transport->headers;
        } else {
            failure = fetchRequestState->transport->failure;
            if (!failure.has_value()) {
                failure = FetchFailure{
                    .type = "TransportError",
                    .message = "HTTP request ended before response headers",
                };
            }
        }
    }
    fetchRequestState->transport->condition.notify_all();
    JSContext* jsContext = fetchRequestState->jsContext;
    ScopedJSValue callback(jsContext, undefinedValue);
    if (reading) {
        fetchRequestState->readID = 0;
        callback.value = std::exchange(fetchRequestState->onRead, undefinedValue);
    } else {
        fetchRequestState->headersID = 0;
        fetchRequestState->headersDelivered = !failure.has_value();
        callback.value = std::exchange(fetchRequestState->onHeaders, undefinedValue);
    }
    if (JS_IsUndefined(callback.value)) {
        return {};
    }
    ScopedJSValue failureValue(jsContext, nullValue);
    ScopedJSValue data(jsContext, nullValue);
    if (failure.has_value()) {
        failureValue.value = JS_NewObjectProto(jsContext, nullValue);
        if (!JS_IsException(failureValue.value)) {
            if (!setstringfield(failureValue, "type", failure.value().type) || !setstringfield(failureValue, "message", failure.value().message)) {
                return errors::Error{
                    .errorCode = ErrorCode::JSException,
                    .errorMessage = "Could not convert HTTP completion data",
                };
            }
        }
    } else if (reading && !done) {
        const ScopedJSValue buffer(jsContext, JS_NewArrayBufferCopy(jsContext, chunk.data(), chunk.size()));
        if (!JS_IsException(buffer.value)) {
            // QuickJS-NG's public helper reads offset/length slots even with argc=1.
            std::array<JSValue, 3> arguments = {buffer.value, undefinedValue, undefinedValue};
            data.value = JS_NewTypedArray(jsContext, static_cast<int>(arguments.size()), arguments.data(), JS_TYPED_ARRAY_UINT8);
        }
    } else if (!reading) {
        data.value = JS_NewObjectProto(jsContext, nullValue);
        ScopedJSValue pairs(jsContext, JS_NewArray(jsContext));
        if (!JS_IsException(data.value) && !JS_IsException(pairs.value)) {
            uint32_t index = 0;
            for (const std::pair<std::string, std::string>& header : metadata.headers) {
                ScopedJSValue pair(jsContext, JS_NewArray(jsContext));
                if (JS_IsException(pair.value)) {
                    break;
                }
                const std::array<std::string_view, 2> fields = {header.first, header.second};
                for (uint32_t field = 0; field < fields.size(); field++) {
                    const std::string_view text = fields.at(field);
                    const JSValue value = JS_NewStringLen(jsContext, text.data(), text.size());
                    if (JS_IsException(value) || JS_DefinePropertyValueUint32(jsContext, pair.value, field, value, JS_PROP_C_W_E) < 0) {
                        break;
                    }
                }
                if (JS_HasException(jsContext) || JS_DefinePropertyValueUint32(jsContext, pairs.value, index, pair.release(), JS_PROP_C_W_E) < 0) {
                    break;
                }
                index++;
            }
            if (!JS_HasException(jsContext) && JS_DefinePropertyValueStr(jsContext, data.value, "status", JS_NewInt32(jsContext, metadata.status), JS_PROP_C_W_E) >= 0) {
                JS_DefinePropertyValueStr(jsContext, data.value, "headers", pairs.release(), JS_PROP_C_W_E);
            }
        }
    }
    if (JS_HasException(jsContext)) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "Could not convert HTTP completion data",
        };
    }
    std::array<JSValue, 2> arguments = {failureValue.value, data.value};
    const ScopedJSValue result(jsContext, JS_Call(jsContext, callback.value, undefinedValue, static_cast<int>(arguments.size()), arguments.data()));
    if (JS_IsException(result.value)) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "HTTP completion callback threw",
        };
    }
    return {};
}

bool isToken(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) {
            continue;
        }
        if (std::string_view("!#$%&'*+-.^_`|~").find(character) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

JSValue fetchOperation(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int operation, JSValueConst* functionData) {
    const FetchHandle* fetchHandle = static_cast<FetchHandle*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
    if (fetchHandle == nullptr || fetchHandle->fetchImpl == nullptr) {
        return JS_ThrowInternalError(jsContext, "Missing native Fetch instance");
    }
    if (operation == 0 && argc >= 5) {
        return fetchHandle->fetchImpl->start(jsContext, argv[0], argv[1], argv[2], argv[3], argv[4]);
    }
    if (operation == 1 && argc >= 2) {
        return fetchHandle->fetchImpl->read(jsContext, argv[0], argv[1]);
    }
    if (operation == 2 && argc >= 1) {
        return fetchHandle->fetchImpl->close(jsContext, argv[0]);
    }
    return JS_ThrowTypeError(jsContext, "Missing native Fetch arguments");
}

} // namespace

JSValue FetchImpl::start(JSContext* jsContext, JSValueConst url, JSValueConst method, JSValueConst headerPairs, JSValueConst bytes, JSValueConst onHeaders) try {
    if (fetchImplState == nullptr || !fetchImplState->active || fetchImplState->jsContext != jsContext) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance is inactive or belongs to another context");
    }
    if (!JS_IsFunction(jsContext, onHeaders)) {
        return JS_ThrowTypeError(jsContext, "Headers completion must be a function");
    }
    if (!JS_IsString(url)) {
        return JS_ThrowTypeError(jsContext, "FetchImpl.start: argument 'url' must be a string");
    }
    const std::optional<std::string> urlText = jsstring(jsContext, url);
    if (!urlText.has_value()) {
        return exceptionValue;
    }
    if (urlText.value().size() > headerLimit) {
        return JS_ThrowRangeError(jsContext, "FetchImpl.start: argument 'url' exceeds 64 KiB");
    }
    if (!JS_IsString(method)) {
        return JS_ThrowTypeError(jsContext, "FetchImpl.start: argument 'method' must be a string");
    }
    const std::optional<std::string> methodText = jsstring(jsContext, method);
    if (!methodText.has_value()) {
        return exceptionValue;
    }
    if (methodText.value().size() > 128) {
        return JS_ThrowRangeError(jsContext, "FetchImpl.start: argument 'method' exceeds 128 bytes");
    }
    const ada::result<ada::url_aggregator> parsed = ada::parse<ada::url_aggregator>(urlText.value(), nullptr);
    if (!parsed.has_value() || (parsed.value().get_protocol() != "http:" && parsed.value().get_protocol() != "https:") || !parsed.value().get_username().empty() || !parsed.value().get_password().empty()) {
        return JS_ThrowTypeError(jsContext, "Only HTTP(S) URLs without credentials are supported");
    }
    if (parsed.value().get_href().size() > headerLimit) {
        return JS_ThrowRangeError(jsContext, "Normalized HTTP URL exceeds 64 KiB");
    }
    httplib::Request request;
    request.method = methodText.value();
    for (const char character : request.method) {
        if (character >= 'a' && character <= 'z') {
            return JS_ThrowTypeError(jsContext, "Native HTTP methods must be normalized to uppercase");
        }
    }
    if (!isToken(request.method) || request.method == "CONNECT" || request.method == "TRACE" || request.method == "TRACK") {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP method");
    }
    request.path = std::string(parsed.value().get_pathname()) + std::string(parsed.value().get_search());
    if (request.path.empty()) {
        request.path = "/";
    }
    int64_t count = 0;
    if (!JS_IsArray(headerPairs)) {
        return JS_ThrowTypeError(jsContext, "Headers must be an array of pairs");
    }
    if (JS_GetLength(jsContext, headerPairs, &count) < 0) {
        return exceptionValue;
    }
    if (count < 0 || count > static_cast<int64_t>(headerLimit)) {
        return JS_ThrowRangeError(jsContext, "Too many HTTP headers");
    }
    std::size_t headerBytes = 0;
    for (int64_t index = 0; index < count; index++) {
        const ScopedJSValue pair(jsContext, JS_GetPropertyUint32(jsContext, headerPairs, static_cast<uint32_t>(index)));
        if (JS_IsException(pair.value)) {
            return exceptionValue;
        }
        int64_t length = 0;
        if (!JS_IsArray(pair.value)) {
            return JS_ThrowTypeError(jsContext, "Expected header pairs");
        }
        if (JS_GetLength(jsContext, pair.value, &length) < 0) {
            return exceptionValue;
        }
        if (length != 2) {
            return JS_ThrowTypeError(jsContext, "Expected two fields per header pair");
        }
        const ScopedJSValue name(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 0));
        const ScopedJSValue value(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 1));
        if (JS_IsException(name.value) || JS_IsException(value.value)) {
            return exceptionValue;
        }
        if (!JS_IsString(name.value)) {
            return JS_ThrowTypeError(jsContext, "FetchImpl.start: header name must be a string");
        }
        const std::optional<std::string> nameText = jsstring(jsContext, name.value);
        if (!nameText.has_value()) {
            return exceptionValue;
        }
        if (nameText.value().size() > headerLimit) {
            return JS_ThrowRangeError(jsContext, "FetchImpl.start: header name exceeds 64 KiB");
        }
        if (!JS_IsString(value.value)) {
            return JS_ThrowTypeError(jsContext, "FetchImpl.start: header value must be a string");
        }
        const std::optional<std::string> valueText = jsstring(jsContext, value.value);
        if (!valueText.has_value()) {
            return exceptionValue;
        }
        if (valueText.value().size() > headerLimit) {
            return JS_ThrowRangeError(jsContext, "FetchImpl.start: header value exceeds 64 KiB");
        }
        std::string lowerName = nameText.value();
        for (char& character : lowerName) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character + ('a' - 'A'));
            }
        }
        if (!isToken(lowerName) || lowerName == "host" || lowerName == "content-length" || lowerName == "transfer-encoding" || lowerName == "connection" || lowerName == "upgrade" || lowerName == "expect" || lowerName.starts_with("proxy-")) {
            return JS_ThrowTypeError(jsContext, "Invalid or transport-controlled HTTP header");
        }
        for (const char character : valueText.value()) {
            if ((static_cast<unsigned char>(character) < 32 && character != '\t') || character == 127) {
                return JS_ThrowTypeError(jsContext, "Invalid HTTP header value");
            }
        }
        const std::array<const std::string*, 2> headerFields = {&lowerName, &valueText.value()};
        for (const std::string* field : headerFields) {
            if (field->size() > headerLimit - headerBytes) {
                return JS_ThrowRangeError(jsContext, "HTTP headers exceed 64 KiB");
            }
            headerBytes += field->size();
        }
        request.headers.insert({lowerName, valueText.value()});
    }
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t elementSize = 0;
    const ScopedJSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, bytes, &offset, &length, &elementSize));
    if (JS_IsException(buffer.value)) {
        return exceptionValue;
    }
    if (elementSize != 1 || length > bodyLimit || (length != 0 && (request.method == "GET" || request.method == "HEAD"))) {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP body type, size, or method");
    }
    if (length != 0) {
        std::size_t bufferLength = 0;
        const uint8_t* data = JS_GetArrayBuffer(jsContext, &bufferLength, buffer.value);
        if (data == nullptr) {
            return exceptionValue;
        }
        if (offset > bufferLength || length > bufferLength - offset) {
            return JS_ThrowRangeError(jsContext, "HTTP body is outside its buffer");
        }
        request.body.assign(data + offset, data + offset + length);
    }
    // Property getters above can reenter JS and clear/start requests.
    if (!fetchImplState->active) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance was cleared");
    }
    if (fetchImplState->requests.size() >= 64 || fetchImplState->lastRequestID == std::numeric_limits<int64_t>::max()) {
        return JS_ThrowRangeError(jsContext, "HTTP request limit or IDs exhausted");
    }
    const std::shared_ptr<FetchRequestState> fetchRequestState = std::make_shared<FetchRequestState>();
    fetchRequestState->jsContext = jsContext;
    fetchRequestState->requestID = ++fetchImplState->lastRequestID;
    fetchRequestState->onHeaders = JS_DupValue(jsContext, onHeaders);
    fetchRequestState->transport = std::make_unique<Transport>();
    fetchRequestState->transport->scheduler = fetchImplState->scheduler;
    fetchRequestState->transport->client = std::make_unique<httplib::Client>(parsed.value().get_origin());
    httplib::Client& client = *fetchRequestState->transport->client;
    if (!client.is_valid()) {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP origin or unavailable TLS backend");
    }
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);
    client.set_write_timeout(30, 0);
    client.set_follow_location(false);
    client.set_keep_alive(false);
    client.set_path_encode(false);
    client.set_decompress(true);
    client.enable_server_certificate_verification(true);
    client.enable_server_hostname_verification(true);
    const ScopedJSValue handle(jsContext, JS_NewObjectProtoClass(jsContext, nullValue, getClassID(JS_GetRuntime(jsContext))));
    if (JS_IsException(handle.value)) {
        return exceptionValue;
    }
    std::unique_ptr<FetchHandle> fetchHandle = std::make_unique<FetchHandle>();
    fetchHandle->owner = fetchImplState;
    fetchHandle->fetchRequestState = fetchRequestState;
    JS_SetOpaque(handle.value, fetchHandle.release());
    try {
        fetchImplState->requests.insert_or_assign(fetchRequestState->requestID, fetchRequestState);
        const std::weak_ptr<FetchRequestState> weakFetchRequestState = fetchRequestState;
        const std::expected<int64_t, errors::Error> callbackIDResult = fetchImplState->scheduler->registerCallback([weakFetchRequestState]() -> errors::Error {
            const std::shared_ptr<FetchRequestState> fetchRequestState = weakFetchRequestState.lock();
            if (fetchRequestState == nullptr) {
                return {};
            }
            return deliver(fetchRequestState, false);
        },
                                                                                                               std::nullopt);
        if (!callbackIDResult.has_value()) {
            throw std::runtime_error(callbackIDResult.error().toString());
        }
        fetchRequestState->headersID = callbackIDResult.value();
        fetchRequestState->transport->headersID = fetchRequestState->headersID;
        fetchRequestState->transport->start(request);
    } catch (...) {
        closeRequest(fetchRequestState, fetchImplState);
        throw;
    }
    return JS_DupValue(jsContext, handle.value);
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP start failure");
}

JSValue FetchImpl::read(JSContext* jsContext, JSValueConst request, JSValueConst onRead) try {
    if (fetchImplState == nullptr || !fetchImplState->active || fetchImplState->jsContext != jsContext) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance is inactive or belongs to another context");
    }
    const std::shared_ptr<FetchRequestState> fetchRequestState = getRequest(jsContext, request, fetchImplState);
    if (fetchRequestState == nullptr) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return JS_ThrowTypeError(jsContext, "HTTP request is closed");
    }
    if (fetchRequestState->closed || !fetchRequestState->headersDelivered || !JS_IsUndefined(fetchRequestState->onRead)) {
        return JS_ThrowTypeError(jsContext, "HTTP read requires headers and no outstanding read");
    }
    if (!JS_IsFunction(jsContext, onRead)) {
        return JS_ThrowTypeError(jsContext, "Read completion must be a function");
    }
    fetchRequestState->onRead = JS_DupValue(jsContext, onRead);
    try {
        const std::weak_ptr<FetchRequestState> weakFetchRequestState = fetchRequestState;
        const std::expected<int64_t, errors::Error> callbackIDResult = fetchImplState->scheduler->registerCallback([weakFetchRequestState]() -> errors::Error {
            const std::shared_ptr<FetchRequestState> fetchRequestState = weakFetchRequestState.lock();
            if (fetchRequestState == nullptr) {
                return {};
            }
            return deliver(fetchRequestState, true);
        },
                                                                                                               std::nullopt);
        if (!callbackIDResult.has_value()) {
            throw std::runtime_error(callbackIDResult.error().toString());
        }
        fetchRequestState->readID = callbackIDResult.value();
        bool ready = false;
        {
            const std::lock_guard<std::mutex> lock(fetchRequestState->transport->mutex);
            fetchRequestState->transport->readID = fetchRequestState->readID;
            ready = !fetchRequestState->transport->chunks.empty() || fetchRequestState->transport->finished;
        }
        if (ready) {
            const errors::Error error = fetchImplState->scheduler->markReady(fetchRequestState->readID);
            if (error.errorCode) {
                throw std::runtime_error(error.toString());
            }
        }
    } catch (...) {
        const errors::Error error = fetchImplState->scheduler->unregisterCallback(fetchRequestState->readID);
        if (error.errorCode) {
            std::terminate();
        }
        JS_FreeValue(jsContext, std::exchange(fetchRequestState->onRead, undefinedValue));
        throw;
    }
    return undefinedValue;
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP read failure");
}

JSValue FetchImpl::close(JSContext* jsContext, JSValueConst request) try {
    if (fetchImplState == nullptr || fetchImplState->jsContext != jsContext) {
        return JS_ThrowTypeError(jsContext, "Native Fetch belongs to another context");
    }
    const std::shared_ptr<FetchRequestState> fetchRequestState = getRequest(jsContext, request, fetchImplState);
    if (fetchRequestState == nullptr) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return undefinedValue;
    }
    if (fetchImplState->active) {
        closeRequest(fetchRequestState, fetchImplState);
    }
    return undefinedValue;
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP close failure");
}

JSModuleDef* FetchImpl::createModule(JSContext* jsContext, Scheduler& scheduler) try {
    JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
    const JSClassID classID = getClassID(jsRuntime);
    if (!JS_IsRegisteredClass(jsRuntime, classID)) {
        const JSClassDef definition = {
            .class_name = "FetchImpl",
            .finalizer = finalizeHandle,
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, classID, &definition) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
    }
    const std::shared_ptr<FetchImplState> fetchImplState = std::make_shared<FetchImplState>();
    fetchImplState->jsContext = jsContext;
    fetchImplState->scheduler = &scheduler;
    std::unique_ptr<FetchHandle> fetchHandle = std::make_unique<FetchHandle>();
    fetchHandle->fetchImpl = std::make_unique<FetchImpl>();
    fetchHandle->fetchImpl->fetchImplState = fetchImplState;
    ScopedJSValue object(jsContext, JS_NewObjectProtoClass(jsContext, nullValue, classID));
    if (JS_IsException(object.value)) {
        return nullptr;
    }
    JS_SetOpaque(object.value, fetchHandle.release());
    const std::array<const char*, 3> names = {"start", "read", "close"};
    const std::array<int, 3> argumentCounts = {5, 2, 1};
    for (std::size_t index = 0; index < names.size(); index++) {
        JSValue functionData = object.value;
        const JSValue function = JS_NewCFunctionData(jsContext, fetchOperation, argumentCounts.at(index), static_cast<int>(index), 1, &functionData);
        if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, object.value, names.at(index), function, JS_PROP_C_W_E) < 0) {
            return nullptr;
        }
    }
    const std::expected<int64_t, errors::Error> cleanupIDResult = scheduler.registerCleanup([fetchImplState]() -> errors::Error {
        fetchImplState->active = false;
        std::map<int64_t, std::shared_ptr<FetchRequestState>> requests;
        requests.swap(fetchImplState->requests);
        errors::Error firstError;
        for (const std::pair<const int64_t, std::shared_ptr<FetchRequestState>>& entry : requests) {
            try {
                closeRequest(entry.second, fetchImplState);
            } catch (const std::exception& exception) {
                if (!firstError.errorCode) {
                    firstError = errors::message(exception.what());
                }
            } catch (...) {
                if (!firstError.errorCode) {
                    firstError = errors::message("Unexpected HTTP cleanup failure");
                }
            }
        }
        return firstError;
    });
    if (!cleanupIDResult.has_value()) {
        JS_ThrowInternalError(jsContext, "%s", cleanupIDResult.error().toString().c_str());
        return nullptr;
    }
    fetchImplState->active = true;
    JSModuleDef* module = JS_NewCModule(jsContext, "amj:fetch_impl", [](JSContext* jsContext, JSModuleDef* module) -> int {
        const JSValue fetchImpl = JS_GetModulePrivateValue(jsContext, module);
        if (JS_IsException(fetchImpl)) {
            return -1;
        }
        return JS_SetModuleExport(jsContext, module, "fetchImpl", fetchImpl);
    });
    if (module == nullptr || JS_AddModuleExport(jsContext, module, "fetchImpl") < 0) {
        return nullptr;
    }
    if (JS_SetModulePrivateValue(jsContext, module, object.release()) < 0) {
        return nullptr;
    }
    return module;
} catch (const std::bad_alloc&) {
    JS_ThrowOutOfMemory(jsContext);
    return nullptr;
} catch (const std::exception& exception) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "%s", exception.what());
    }
    return nullptr;
} catch (...) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "Unexpected FetchImpl module creation failure");
    }
    return nullptr;
}

} // namespace amj::javascript
