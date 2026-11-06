/*
 * Copyright (c) 2013 GitHub Inc.
 * Copyright (c) 2019 Hannes Rantzsch, René Meusel
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

#if defined(_WIN32)

#include "keyring.hpp"

#include "errors/errors.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

// clang-format off
// windows.h must precede wincred.h and wincrypt.h; wincrypt.h supplies DPAPI types.
#include <windows.h> // IWYU pragma: keep
#include <wincred.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <errhandlingapi.h>
#include <minwindef.h>
#include <stringapiset.h>
#include <winbase.h>
#include <winerror.h>
#include <winnls.h>
// clang-format on

namespace amj::keyring {
namespace errors = amj::errors;

namespace {

constexpr std::size_t maxEncryptedSize = CRED_MAX_CREDENTIAL_BLOB_SIZE + CRED_MAX_ATTRIBUTES * CRED_MAX_VALUE_SIZE;

// DPAPI owns these allocations. Wipe decrypted material before releasing it,
// including when constructing the returned std::string throws.
struct LocalData {
    DATA_BLOB blob = {};

    ~LocalData() {
        if (blob.pbData != nullptr) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
    LocalData() = default;
    LocalData(const LocalData&) = delete;
    LocalData& operator=(const LocalData&) = delete;
};

errors::Error makeError(DWORD code) {
    errors::Error error;
    error.errorCode = errors::ErrorCode::GenericError;
    if (code == ERROR_NOT_FOUND) {
        error.errorCode = ErrorCode::NotFound;
    }
    error.detail = static_cast<int>(code);
    error.errorMessage = "Windows error " + std::to_string(code);
    // System text uses the active code page; the numeric code is always retained.
    std::array<char, 1024> buffer = {};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    if (length != 0) {
        error.errorMessage.assign(buffer.data(), length);
    }
    return error;
}

std::expected<std::wstring, errors::Error> utf8ToWide(const std::string& value) {
    if (value.find('\0') != std::string::npos || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(makeError(ERROR_INVALID_PARAMETER));
    }
    if (value.empty()) {
        return L"";
    }
    const int size = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, nullptr, 0);
    if (length == 0) {
        return std::unexpected(makeError(GetLastError()));
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, result.data(), length) == 0) {
        return std::unexpected(makeError(GetLastError()));
    }
    return result;
}

std::expected<std::wstring, errors::Error> makeTargetName(const std::string& service, const std::string& user) {
    std::expected<std::wstring, errors::Error> targetResult = utf8ToWide(service + "/" + user);
    if (!targetResult.has_value()) {
        return std::unexpected(targetResult.error());
    }
    if (targetResult.value().size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
        errors::Error error = makeError(ERROR_INVALID_PARAMETER);
        error.errorMessage = "Credential target name too long";
        return std::unexpected(error);
    }
    return targetResult;
}

} // namespace

errors::Error save(const std::string& service, const std::string& user, const std::string& value) {
    std::expected<std::wstring, errors::Error> targetResult = makeTargetName(service, user);
    if (!targetResult.has_value()) {
        return targetResult.error();
    }
    std::expected<std::wstring, errors::Error> usernameResult = utf8ToWide(user);
    if (!usernameResult.has_value()) {
        return usernameResult.error();
    }
    // DPAPI only adds overhead, so oversized plaintext cannot possibly fit.
    if (value.size() > maxEncryptedSize) {
        return errors::Error{
            .errorCode = ErrorCode::SecretTooLong,
            .errorMessage = "Secret exceeds the 18,944-byte encrypted credential capacity",
        };
    }

    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = targetResult.value().data();
    credential.UserName = usernameResult.value().data();
    credential.Persist = CRED_PERSIST_ENTERPRISE;

    std::vector<BYTE> secret(value.begin(), value.end());
    std::vector<BYTE> encrypted;
    std::vector<std::wstring> keywords;
    std::vector<CREDENTIAL_ATTRIBUTEW> attributes;
    if (value.size() <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        credential.CredentialBlobSize = static_cast<DWORD>(value.size());
        credential.CredentialBlob = secret.data();
    } else {
        DATA_BLOB input = {};
        input.cbData = static_cast<DWORD>(value.size());
        input.pbData = secret.data();
        LocalData output;
        if (CryptProtectData(&input, L"keyring encrypted data", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob) == FALSE) {
            return makeError(GetLastError());
        }
        if (output.blob.cbData > maxEncryptedSize) {
            return errors::Error{
                .errorCode = ErrorCode::SecretTooLong,
                .errorMessage = "DPAPI-encrypted secret exceeds 18,944 bytes",
            };
        }
        if (output.blob.pbData == nullptr || output.blob.cbData <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
            return makeError(ERROR_INVALID_DATA);
        }
        const std::span<const BYTE> encryptedBytes(output.blob.pbData, output.blob.cbData);
        encrypted.assign(encryptedBytes.begin(), encryptedBytes.end());
        credential.CredentialBlobSize = CRED_MAX_CREDENTIAL_BLOB_SIZE;
        credential.CredentialBlob = encrypted.data();
        const std::size_t remaining = encrypted.size() - CRED_MAX_CREDENTIAL_BLOB_SIZE;
        const std::size_t count = (remaining + CRED_MAX_VALUE_SIZE - 1) / CRED_MAX_VALUE_SIZE;
        keywords.reserve(count);
        attributes.resize(count);
        std::size_t offset = CRED_MAX_CREDENTIAL_BLOB_SIZE;
        for (std::size_t index = 0; index < count; ++index) {
            // Keywords must be distinct. Their prefix also identifies our encrypted format.
            keywords.push_back(L"keyring.dpapi." + std::to_wstring(index));
            CREDENTIAL_ATTRIBUTEW& attribute = attributes.at(index);
            attribute.Keyword = keywords.at(index).data();
            attribute.ValueSize = static_cast<DWORD>(std::min<std::size_t>(CRED_MAX_VALUE_SIZE, encrypted.size() - offset));
            attribute.Value = &encrypted.at(offset);
            offset += attribute.ValueSize;
        }
        credential.AttributeCount = static_cast<DWORD>(attributes.size());
        credential.Attributes = attributes.data();
    }
    if (CredWriteW(&credential, 0) == FALSE) {
        return makeError(GetLastError());
    }
    return {};
}

std::expected<std::string, errors::Error> load(const std::string& service, const std::string& user) {
    const std::expected<std::wstring, errors::Error> targetResult = makeTargetName(service, user);
    if (!targetResult.has_value()) {
        return std::unexpected(targetResult.error());
    }
    CREDENTIALW* rawCredential = nullptr;
    if (CredReadW(targetResult.value().c_str(), CRED_TYPE_GENERIC, 0, &rawCredential) == FALSE) {
        return std::unexpected(makeError(GetLastError()));
    }
    const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> ownedCredential(rawCredential, &CredFree);
    if (rawCredential == nullptr) {
        return std::unexpected(makeError(ERROR_INVALID_DATA));
    }
    const CREDENTIALW& credential = *rawCredential;
    if (credential.CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE || (credential.CredentialBlobSize != 0 && credential.CredentialBlob == nullptr)) {
        return std::unexpected(makeError(ERROR_INVALID_DATA));
    }
    std::vector<BYTE> data;
    if (credential.CredentialBlobSize != 0) {
        const std::span<const BYTE> blob(credential.CredentialBlob, credential.CredentialBlobSize);
        data.assign(blob.begin(), blob.end());
    }
    if (credential.AttributeCount == 0) {
        return std::string(data.begin(), data.end());
    }
    if (credential.AttributeCount > CRED_MAX_ATTRIBUTES || credential.Attributes == nullptr || credential.CredentialBlobSize != CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return std::unexpected(makeError(ERROR_INVALID_DATA));
    }
    const std::span<const CREDENTIAL_ATTRIBUTEW> attributes(credential.Attributes, credential.AttributeCount);
    std::vector<bool> seen(attributes.size(), false);
    std::vector<std::vector<BYTE>> chunks(attributes.size());
    // Do not assume Credential Manager preserves attribute order.
    for (const CREDENTIAL_ATTRIBUTEW& attribute : attributes) {
        if (attribute.Keyword == nullptr || attribute.Value == nullptr || attribute.ValueSize == 0 || attribute.ValueSize > CRED_MAX_VALUE_SIZE) {
            return std::unexpected(makeError(ERROR_INVALID_DATA));
        }
        bool recognized = false;
        for (std::size_t index = 0; index < chunks.size(); ++index) {
            if (std::wstring(L"keyring.dpapi." + std::to_wstring(index)) != attribute.Keyword) {
                continue;
            }
            if (seen.at(index)) {
                return std::unexpected(makeError(ERROR_INVALID_DATA));
            }
            seen.at(index) = true;
            const std::span<const BYTE> chunk(attribute.Value, attribute.ValueSize);
            chunks.at(index).assign(chunk.begin(), chunk.end());
            recognized = true;
            break;
        }
        if (!recognized) {
            return std::unexpected(makeError(ERROR_INVALID_DATA));
        }
    }
    for (const std::vector<BYTE>& chunk : chunks) {
        data.insert(data.end(), chunk.begin(), chunk.end());
    }
    DATA_BLOB input = {};
    input.cbData = static_cast<DWORD>(data.size());
    input.pbData = data.data();
    LocalData output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob) == FALSE) {
        return std::unexpected(makeError(GetLastError()));
    }
    if (output.blob.cbData == 0) {
        return "";
    }
    if (output.blob.pbData == nullptr) {
        return std::unexpected(makeError(ERROR_INVALID_DATA));
    }
    const std::span<const BYTE> plaintext(output.blob.pbData, output.blob.cbData);
    return std::string(plaintext.begin(), plaintext.end());
}

errors::Error remove(const std::string& service, const std::string& user) {
    const std::expected<std::wstring, errors::Error> targetResult = makeTargetName(service, user);
    if (!targetResult.has_value()) {
        return targetResult.error();
    }
    if (CredDeleteW(targetResult.value().c_str(), CRED_TYPE_GENERIC, 0) == FALSE) {
        return makeError(GetLastError());
    }
    return {};
}

std::expected<bool, errors::Error> isAvailable() {
    return true;
}

} // namespace amj::keyring

#endif // _WIN32
