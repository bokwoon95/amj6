/*
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

#pragma once

#include "errors/errors.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <type_traits>

namespace amj::keyring {
namespace errors = amj::errors;

enum class ErrorCode : std::uint8_t {
    NotFound = 1,
    Unavailable,
    SecretTooLong,
    AccessDenied,
};

[[nodiscard]] std::error_code make_error_code(ErrorCode errorCode);

// Native OS error numbers, when available, are retained as int in errors::Error::detail.

// These operations never acquire application locks. Cooperating callers must use
// the same canonical lock name for each (service, user) identity, holding
// ownership across the entire read/refresh/save transaction.

// Blocking operations; unlocking the OS credential store may require user interaction.
// Namespace service names to avoid collisions with other applications.
// A missing entry reports NotFound; an empty secret is a successful empty string.
[[nodiscard]] std::expected<std::string, errors::Error> load(const std::string& service, const std::string& user);

// Inserts or overwrites an entry. Windows stores values up to 2,560 bytes directly
// in Credential Manager; larger values are DPAPI-encrypted and split across the
// blob and attributes. Ciphertext must fit within 18,944 bytes (plaintext less).
// Linux retains libsecret's UTF-8, NUL-terminated secret text semantics.
[[nodiscard]] errors::Error save(const std::string& service, const std::string& user, const std::string& value);

// Deleting a missing entry reports NotFound.
[[nodiscard]] errors::Error remove(const std::string& service, const std::string& user);

// Probes the native Linux/macOS credential service. Windows Credential Manager is a
// built-in component, so availability always returns true there.
[[nodiscard]] std::expected<bool, errors::Error> isAvailable();

} // namespace amj::keyring

namespace keyring = amj::keyring;

namespace std {

template <>
struct is_error_code_enum<keyring::ErrorCode> : true_type {};

} // namespace std
