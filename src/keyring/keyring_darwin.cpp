/*
 * Copyright (c) 2013 GitHub Inc.
 * Copyright (c) 2019 Hannes Rantzsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#if defined(__APPLE__)
#include <TargetConditionals.h> // IWYU pragma: keep
#if TARGET_OS_OSX

#include "keyring.hpp"

#include "errors/errors.hpp"

#include <any>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFString.h>
#include <MacTypes.h>
#include <Security/SecBase.h>
#include <Security/SecItem.h>

namespace amj::keyring {
namespace errors = amj::errors;

namespace {

// Owns objects returned under Core Foundation's Create/Copy rule.
// The wrapper stays at a stable address; its unique_ptr transfers ownership.
template <typename T> struct ScopedCFRef {
    T ownedObject = nullptr;

    ScopedCFRef() = default;
    ~ScopedCFRef() {
        if (ownedObject != nullptr) {
            CFRelease(ownedObject);
        }
    }
    ScopedCFRef(const ScopedCFRef&) = delete;
    ScopedCFRef& operator=(const ScopedCFRef&) = delete;
};

template <typename T> std::unique_ptr<ScopedCFRef<T>> createScopedCFRef(T object) {
    // Guard the incoming reference even if allocating the wrapper throws.
    ScopedCFRef<T> reference;
    reference.ownedObject = object;
    std::unique_ptr<ScopedCFRef<T>> scopedCFRef = std::make_unique<ScopedCFRef<T>>();
    scopedCFRef->ownedObject = reference.ownedObject;
    reference.ownedObject = nullptr;
    return scopedCFRef;
}

errors::Error makeError(OSStatus status) {
    errors::Error error;
    error.detail = std::make_shared<const std::any>(static_cast<int>(status));
    error.errorCode = errors::ErrorCode::GenericError;
    if (status == errSecItemNotFound) {
        error.errorCode = ErrorCode::NotFound;
    } else if (status == errSecUserCanceled || status == errSecAuthFailed ||
               status == errSecInteractionRequired) {
        error.errorCode = ErrorCode::AccessDenied;
    }
    error.errorMessage =
        std::make_shared<const std::string>("Security error " + std::to_string(status));
    const std::unique_ptr<ScopedCFRef<CFStringRef>> message =
        createScopedCFRef(SecCopyErrorMessageString(status, nullptr));
    if (message->ownedObject == nullptr) {
        return error;
    }
    const CFIndex length = CFStringGetLength(message->ownedObject);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (capacity < 0 || capacity == std::numeric_limits<CFIndex>::max()) {
        return error;
    }
    std::vector<char> buffer(static_cast<std::size_t>(capacity + 1), '\0');
    if (CFStringGetCString(message->ownedObject, buffer.data(), capacity + 1,
            kCFStringEncodingUTF8) != 0) {
        error.errorMessage = std::make_shared<const std::string>(buffer.data());
    }
    return error;
}

// Returns an owned dictionary under Core Foundation's Create rule.
std::expected<CFMutableDictionaryRef, errors::Error> createQuery(const std::string& service,
    const std::string& user) {
    const std::unique_ptr<ScopedCFRef<CFStringRef>> cfService = createScopedCFRef(
        CFStringCreateWithCString(kCFAllocatorDefault, service.c_str(), kCFStringEncodingUTF8)
    );
    const std::unique_ptr<ScopedCFRef<CFStringRef>> cfUser = createScopedCFRef(
        CFStringCreateWithCString(kCFAllocatorDefault, user.c_str(), kCFStringEncodingUTF8)
    );
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr || cfService->ownedObject == nullptr || cfUser->ownedObject == nullptr) {
        if (query != nullptr) {
            CFRelease(query);
        }
        return std::unexpected(errors::message("Failed to create credential query"));
    }
    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrAccount, cfUser->ownedObject);
    CFDictionaryAddValue(query, kSecAttrService, cfService->ownedObject);
    return query;
}

} // namespace

errors::Error save(const std::string& service, const std::string& user, const std::string& value) {
    const std::expected<CFMutableDictionaryRef, errors::Error> queryResult =
        createQuery(service, user);
    if (!queryResult.has_value()) {
        return queryResult.error();
    }
    const std::unique_ptr<ScopedCFRef<CFMutableDictionaryRef>> query =
        createScopedCFRef(queryResult.value());
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<CFIndex>::max())) {
        return errors::message("Secret too long");
    }
    const std::vector<UInt8> secret(value.begin(), value.end());
    const std::unique_ptr<ScopedCFRef<CFDataRef>> data = createScopedCFRef(
        CFDataCreate(kCFAllocatorDefault, secret.data(), static_cast<CFIndex>(secret.size()))
    );
    if (data->ownedObject == nullptr) {
        return errors::message("Failed to create secret data");
    }
    CFDictionaryAddValue(query->ownedObject, kSecValueData, data->ownedObject);
    OSStatus status = SecItemAdd(query->ownedObject, nullptr);
    if (status == errSecDuplicateItem) {
        CFDictionaryRemoveValue(query->ownedObject, kSecValueData);
        const std::unique_ptr<ScopedCFRef<CFMutableDictionaryRef>> attributes =
            createScopedCFRef(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        if (attributes->ownedObject == nullptr) {
            return errors::message("Failed to create secret attributes");
        }
        CFDictionaryAddValue(attributes->ownedObject, kSecValueData, data->ownedObject);
        status = SecItemUpdate(query->ownedObject, attributes->ownedObject);
    }
    if (status != errSecSuccess) {
        return makeError(status);
    }
    return {};
}

std::expected<std::string, errors::Error> load(const std::string& service,
    const std::string& user) {
    const std::expected<CFMutableDictionaryRef, errors::Error> queryResult =
        createQuery(service, user);
    if (!queryResult.has_value()) {
        return std::unexpected(queryResult.error());
    }
    const std::unique_ptr<ScopedCFRef<CFMutableDictionaryRef>> query =
        createScopedCFRef(queryResult.value());
    CFDictionaryAddValue(query->ownedObject, kSecReturnData, kCFBooleanTrue);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query->ownedObject, &result);
    const std::unique_ptr<ScopedCFRef<CFTypeRef>> ownedResult = createScopedCFRef(result);
    if (status != errSecSuccess) {
        return std::unexpected(makeError(status));
    }
    if (result == nullptr || CFGetTypeID(result) != CFDataGetTypeID()) {
        return std::unexpected(errors::message("Invalid secret data type"));
    }
    const CFDataRef data = static_cast<CFDataRef>(result);
    const CFIndex length = CFDataGetLength(data);
    if (length == 0) {
        return "";
    }
    const UInt8* bytes = CFDataGetBytePtr(data);
    if (length < 0 || bytes == nullptr) {
        return std::unexpected(errors::message("Invalid secret data buffer"));
    }
    const std::span<const UInt8> plaintext(bytes, static_cast<std::size_t>(length));
    return std::string(plaintext.begin(), plaintext.end());
}

errors::Error remove(const std::string& service, const std::string& user) {
    const std::expected<CFMutableDictionaryRef, errors::Error> queryResult =
        createQuery(service, user);
    if (!queryResult.has_value()) {
        return queryResult.error();
    }
    const std::unique_ptr<ScopedCFRef<CFMutableDictionaryRef>> query =
        createScopedCFRef(queryResult.value());
    const OSStatus status = SecItemDelete(query->ownedObject);
    if (status != errSecSuccess) {
        return makeError(status);
    }
    return {};
}

std::expected<bool, errors::Error> isAvailable() {
    const std::expected<CFMutableDictionaryRef, errors::Error> queryResult =
        createQuery("keyring.availability_check_service", "availability_check_account");
    if (!queryResult.has_value()) {
        errors::Error error = queryResult.error();
        error.errorCode = ErrorCode::Unavailable;
        return std::unexpected(error);
    }
    const std::unique_ptr<ScopedCFRef<CFMutableDictionaryRef>> query =
        createScopedCFRef(queryResult.value());
    CFDictionaryAddValue(query->ownedObject, kSecReturnData, kCFBooleanFalse);
    const OSStatus status = SecItemCopyMatching(query->ownedObject, nullptr);
    if (status == errSecSuccess || status == errSecItemNotFound) {
        return true;
    }
    errors::Error error = makeError(status);
    error.errorCode = ErrorCode::Unavailable;
    return std::unexpected(error);
}

} // namespace amj::keyring

#endif // TARGET_OS_OSX
#endif // defined(__APPLE__)
