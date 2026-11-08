#include "crypto_impl.hpp"

#include "javascript.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <quickjs.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace this_thread = std::this_thread;

namespace {

struct DigestWork {
    uv_work_t uvWork = {};
    Crypto* crypto = nullptr; // Only read/written on the owner thread.
    const EVP_MD* digestType = nullptr;
    std::vector<uint8_t> input = {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> output = {};
    unsigned int outputLength = 0;
    int digestStatus = 0;
    unsigned long opensslError = 0;
};

void digestDidFinish(uv_work_t* uvWork, int status) noexcept {
    try {
        const std::unique_ptr<DigestWork> digestWork(static_cast<DigestWork*>(uvWork->data));
        Crypto* crypto = digestWork->crypto;
        if (crypto == nullptr) {
            return; // Shutdown detached this request; no JS dependencies remain.
        }
        if (this_thread::get_id() != crypto->ownerThreadID) {
            std::terminate();
        }
        JSContext* jsContext = crypto->jsContext;
        const std::map<uv_work_t*, OwnedJSValue>::node_type completionNode = crypto->pendingDigests.extract(uvWork);
        if (completionNode.empty()) {
            std::terminate();
        }
        const OwnedJSValue& completion = completionNode.mapped();
        digestWork->crypto = nullptr;

        JSValue outputValue = JS_EXCEPTION;
        try {
            if (status < 0) {
                throw std::runtime_error(uv_strerror(status));
            }
            if (digestWork->digestStatus != 1) {
                const char* reason = ERR_reason_error_string(digestWork->opensslError);
                if (reason != nullptr) {
                    throw std::runtime_error(reason);
                }
                throw std::runtime_error("OpenSSL digest failed");
            }
            if (digestWork->outputLength > digestWork->output.size()) {
                throw std::runtime_error("OpenSSL returned an invalid digest length");
            }
            outputValue = JS_NewArrayBufferCopy(jsContext, digestWork->output.data(), digestWork->outputLength);
        } catch (const std::bad_alloc&) {
            JS_ThrowOutOfMemory(jsContext);
        } catch (const std::exception& exception) {
            JS_ThrowInternalError(jsContext, "cryptoDigest: %s", exception.what());
        } catch (...) {
            JS_ThrowInternalError(jsContext, "cryptoDigest: unexpected native failure");
        }
        const OwnedJSValue output(jsContext, outputValue);
        JSValue errorValue = JS_NULL;
        if (JS_IsException(output.get())) {
            errorValue = JS_GetException(jsContext);
            if (!setstringfield(jsContext, errorValue, "name", "OperationError")) {
                JS_FreeValue(jsContext, errorValue);
                errorValue = JS_GetException(jsContext);
            }
        }
        const OwnedJSValue error(jsContext, errorValue);
        std::array<JSValue, 2> arguments = {error.get(), JS_NULL};
        if (!JS_IsException(output.get())) {
            arguments.at(1) = output.get();
        }
        {
            const OwnedJSValue result(jsContext, JS_Call(jsContext, completion.get(), JS_UNDEFINED, static_cast<int>(arguments.size()), arguments.data()));
        }
        crypto->callbackDidRun(jsContext);
    } catch (...) {
        // No C++ exception may escape a libuv callback or a throwing host hook.
        std::terminate();
    }
}

} // namespace

Crypto::Crypto(JSContext* jsContext, uv_loop_t* uvLoop, std::function<void(JSContext*)> callbackDidRun) {
    if (jsContext == nullptr || uvLoop == nullptr || !callbackDidRun) {
        throw std::invalid_argument("Crypto requires a context, initialized loop, and completion hook");
    }
    this->jsRuntime = JS_GetRuntime(jsContext);
    this->jsContext = jsContext;
    this->uvLoop = uvLoop;
    this->callbackDidRun = std::move(callbackDidRun);
    this->ownerThreadID = this_thread::get_id();
}

Crypto::~Crypto() {
    shutdown();
}

void Crypto::shutdown() {
    if (this_thread::get_id() != ownerThreadID) {
        std::terminate();
    }
    if (shuttingDown) {
        return; // Safe even after the context/runtime have been destroyed.
    }
    shuttingDown = true;
    std::map<uv_work_t*, OwnedJSValue> detached;
    detached.swap(pendingDigests);
    for (const std::pair<uv_work_t* const, OwnedJSValue>& entry : detached) {
        DigestWork* digestWork = static_cast<DigestWork*>(entry.first->data);
        digestWork->crypto = nullptr;
    }
}

JSValue cryptoGetRandomValues(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) {
    try {
        const int arrayType = argc > 0 ? JS_GetTypedArrayType(argv[0]) : -1;
        if (arrayType < JS_TYPED_ARRAY_UINT8C || arrayType > JS_TYPED_ARRAY_BIG_UINT64) {
            OwnedJSValue error(jsContext, JS_NewError(jsContext));
            if (JS_IsException(error.get())) {
                return JS_EXCEPTION;
            }
            if (!setstringfield(jsContext, error.get(), "name", "TypeMismatchError")
                || !setstringfield(jsContext, error.get(), "message", "getRandomValues requires an integer typed array")) {
                return JS_EXCEPTION;
            }
            return JS_Throw(jsContext, error.release());
        }
        std::size_t byteOffset = 0;
        std::size_t byteLength = 0;
        const OwnedJSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, argv[0], &byteOffset, &byteLength, nullptr));
        if (JS_IsException(buffer.get())) {
            return JS_EXCEPTION;
        }
        if (!JS_IsArrayBuffer(buffer.get())) {
            return JS_ThrowTypeError(jsContext, "getRandomValues does not accept shared buffers");
        }
        const int immutable = JS_IsImmutableArrayBuffer(buffer.get());
        if (immutable < 0) {
            return JS_EXCEPTION;
        }
        if (immutable != 0) {
            return JS_ThrowTypeError(jsContext, "getRandomValues cannot modify an immutable buffer");
        }
        if (byteLength > 65536) {
            OwnedJSValue error(jsContext, JS_NewError(jsContext));
            if (JS_IsException(error.get())) {
                return JS_EXCEPTION;
            }
            if (!setstringfield(jsContext, error.get(), "name", "QuotaExceededError")
                || !setstringfield(jsContext, error.get(), "message", "getRandomValues exceeds 65536 bytes")) {
                return JS_EXCEPTION;
            }
            return JS_Throw(jsContext, error.release());
        }
        std::size_t bufferLength = 0;
        uint8_t* bytes = JS_GetArrayBuffer(jsContext, &bufferLength, buffer.get());
        if (bytes == nullptr && JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        if (byteOffset > bufferLength || byteLength > bufferLength - byteOffset) {
            return JS_ThrowTypeError(jsContext, "Invalid random array bounds");
        }
        if (byteLength != 0) {
            if (bytes == nullptr) {
                return JS_ThrowInternalError(jsContext, "Missing random array storage");
            }
            // Bounds are checked above; no JS call occurs while using this pointer.
            const int status = uv_random(nullptr, nullptr, bytes + byteOffset, byteLength, 0, nullptr);
            if (status != 0) {
                return JS_ThrowInternalError(jsContext, "getRandomValues: %s", uv_strerror(status));
            }
        }
        return JS_DupValue(jsContext, argv[0]);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "getRandomValues: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "getRandomValues: unexpected native failure");
    }
}

JSValue cryptoDigest(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, Crypto& crypto) {
    try {
        if (this_thread::get_id() != crypto.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != crypto.jsContext || crypto.shuttingDown) {
            return JS_ThrowInternalError(jsContext, "cryptoDigest: wrong context or service is shut down");
        }
        if (argc < 3 || !JS_IsString(argv[0]) || JS_GetTypedArrayType(argv[1]) != JS_TYPED_ARRAY_UINT8 || !JS_IsFunction(jsContext, argv[2])) {
            return JS_ThrowTypeError(jsContext, "cryptoDigest requires an algorithm, Uint8Array, and completion callback");
        }
        const std::optional<std::string> algorithm = jsstring(jsContext, argv[0]);
        if (!algorithm.has_value()) {
            return JS_EXCEPTION;
        }
        const EVP_MD* digestType = nullptr;
        if (algorithm.value() == "SHA-1") {
            digestType = EVP_sha1();
        } else if (algorithm.value() == "SHA-256") {
            digestType = EVP_sha256();
        } else if (algorithm.value() == "SHA-384") {
            digestType = EVP_sha384();
        } else if (algorithm.value() == "SHA-512") {
            digestType = EVP_sha512();
        } else {
            return JS_ThrowTypeError(jsContext, "Unsupported digest algorithm");
        }
        std::size_t byteOffset = 0;
        std::size_t byteLength = 0;
        const OwnedJSValue buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, argv[1], &byteOffset, &byteLength, nullptr));
        if (JS_IsException(buffer.get())) {
            return JS_EXCEPTION;
        }
        if (!JS_IsArrayBuffer(buffer.get())) {
            return JS_ThrowTypeError(jsContext, "cryptoDigest does not accept shared buffers");
        }
        std::size_t bufferLength = 0;
        const uint8_t* bytes = JS_GetArrayBuffer(jsContext, &bufferLength, buffer.get());
        if (bytes == nullptr && JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        if (byteOffset > bufferLength || byteLength > bufferLength - byteOffset) {
            return JS_ThrowTypeError(jsContext, "Invalid digest input bounds");
        }
        std::unique_ptr<DigestWork> digestWork = std::make_unique<DigestWork>();
        digestWork->digestType = digestType;
        digestWork->input.resize(byteLength);
        if (byteLength != 0) {
            if (bytes == nullptr) {
                return JS_ThrowInternalError(jsContext, "Missing digest input storage");
            }
            // Snapshot the validated slice before returning or invoking any JS.
            std::copy_n(bytes + byteOffset, byteLength, digestWork->input.data());
        }
        digestWork->uvWork.data = digestWork.get();
        OwnedJSValue completion(jsContext, JS_DupValue(jsContext, argv[2]));
        crypto.pendingDigests.insert_or_assign(&digestWork->uvWork, std::move(completion));
        const auto doDigest = [](uv_work_t* uvWork) noexcept {
            DigestWork* digestWork = static_cast<DigestWork*>(uvWork->data);
            // The worker touches native storage only, never crypto/context/JS.
            ERR_clear_error();
            digestWork->digestStatus = EVP_Digest(digestWork->input.data(), digestWork->input.size(), digestWork->output.data(), &digestWork->outputLength, digestWork->digestType, nullptr);
            if (digestWork->digestStatus != 1) {
                digestWork->opensslError = ERR_get_error();
            }
        };
        const int status = uv_queue_work(crypto.uvLoop, &digestWork->uvWork, doDigest, digestDidFinish);
        if (status != 0) {
            crypto.pendingDigests.erase(&digestWork->uvWork);
            return JS_ThrowInternalError(jsContext, "cryptoDigest: uv_queue_work: %s", uv_strerror(status));
        }
        // The map owns completion; after successful submission,
        // digestDidFinish owns and deletes the work allocation.
        DigestWork* submittedWork = digestWork.release();
        submittedWork->crypto = &crypto;
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        return JS_ThrowInternalError(jsContext, "cryptoDigest: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "cryptoDigest: unexpected native failure");
    }
}

JSModuleDef* createCryptoImplModule(JSContext* jsContext, Crypto& crypto) {
    try {
        if (this_thread::get_id() != crypto.ownerThreadID) {
            std::terminate();
        }
        if (jsContext != crypto.jsContext || crypto.shuttingDown) {
            JS_ThrowInternalError(jsContext, "createCryptoImplModule: wrong context or service is shut down");
            return nullptr;
        }
        JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
        JSClassID cryptoClassID = 0;
        JS_NewClassID(jsRuntime, &cryptoClassID);
        const JSClassDef jsClassDef = {
            .class_name = "Crypto",
            .finalizer = nullptr,
            .gc_mark = nullptr, // Callback references belong to the host-owned Crypto.
            .call = nullptr,
            .exotic = nullptr,
        };
        int status = JS_NewClass(jsRuntime, cryptoClassID, &jsClassDef);
        if (status < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
        OwnedJSValue modulePrivateValue(jsContext, JS_NewObjectProtoClass(jsContext, JS_NULL, cryptoClassID));
        if (JS_IsException(modulePrivateValue.get())) {
            return nullptr;
        }
        status = JS_SetOpaque(modulePrivateValue.get(), &crypto);
        if (status < 0) {
            JS_ThrowInternalError(jsContext, "Could not attach Crypto");
            return nullptr;
        }
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:crypto_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            const JSValue randomFunction = JS_NewCFunction(jsContext, cryptoGetRandomValues, "cryptoGetRandomValues", 1);
            if (JS_IsException(randomFunction)) {
                return -1;
            }
            const int status = JS_SetModuleExport(jsContext, jsModuleDef, "cryptoGetRandomValues", randomFunction);
            if (status < 0) {
                return -1;
            }
            const OwnedJSValue modulePrivateValue(jsContext, JS_GetModulePrivateValue(jsContext, jsModuleDef));
            if (JS_IsException(modulePrivateValue.get())) {
                return -1;
            }
            const auto digestCallback = [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) -> JSValue {
                Crypto* crypto = static_cast<Crypto*>(JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0])));
                if (crypto == nullptr) {
                    return JS_EXCEPTION;
                }
                return cryptoDigest(jsContext, thisValue, argc, argv, *crypto);
            };
            JSValueConst functionData = modulePrivateValue.get();
            const JSValue digestFunction = JS_NewCFunctionData2(jsContext, digestCallback, "cryptoDigest", 3, 0, 1, &functionData);
            if (JS_IsException(digestFunction)) {
                return -1;
            }
            return JS_SetModuleExport(jsContext, jsModuleDef, "cryptoDigest", digestFunction);
        });
        if (jsModuleDef == nullptr) {
            return nullptr;
        }
        if (JS_AddModuleExport(jsContext, jsModuleDef, "cryptoGetRandomValues") < 0
            || JS_AddModuleExport(jsContext, jsModuleDef, "cryptoDigest") < 0) {
            return nullptr;
        }
        status = JS_SetModulePrivateValue(jsContext, jsModuleDef, modulePrivateValue.release());
        if (status < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(jsContext);
        return nullptr;
    } catch (const std::exception& exception) {
        JS_ThrowInternalError(jsContext, "createCryptoImplModule: %s", exception.what());
        return nullptr;
    } catch (...) {
        JS_ThrowInternalError(jsContext, "createCryptoImplModule: unexpected native failure");
        return nullptr;
    }
}

} // namespace amj::javascript
