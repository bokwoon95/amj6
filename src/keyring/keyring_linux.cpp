/*
 * Copyright (c) 2013 GitHub Inc.
 * Copyright (c) 2015-2019 Vaclav Slavik
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

#if defined(__linux__)

#include "keyring.hpp"

#include "errors/errors.hpp"

#include <libsecret/secret.h>

#include <expected>
#include <memory>
#include <string>

namespace amj::keyring {
namespace errors = amj::errors;

namespace {

SecretSchema makeSchema() {
    SecretSchema schema = {};
    schema.name = "keyring";
    schema.flags = SECRET_SCHEMA_DONT_MATCH_NAME;
    // SecretSchema's fixed C array has 32 entries; these constant indices are in bounds.
    schema.attributes[0].name = "service";
    schema.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    schema.attributes[1].name = "username";
    schema.attributes[1].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    return schema;
}

errors::Error makeError(GError* error) {
    if (error == nullptr) {
        return errors::Error{
            .errorCode = ErrorCode::NotFound,
            .errorMessage = "Secret not found.",
        };
    }
    errors::Error result = {
        .errorCode = errors::ErrorCode::GenericError,
        .errorMessage = error->message,
        .detail = error->code,
    };
    g_error_free(error);
    return result;
}

} // namespace

errors::Error save(const std::string& service, const std::string& user, const std::string& value) {
    const SecretSchema schema = makeSchema();
    std::string label = service;
    if (!user.empty()) {
        label += " (" + user + ")";
    }
    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr, &error, "service", service.c_str(), "username", user.c_str(), nullptr);
    if (error != nullptr) {
        return makeError(error);
    }
    if (stored == 0) {
        return errors::generic("Failed to store secret.");
    }
    return {};
}

std::expected<std::string, errors::Error> load(const std::string& service, const std::string& user) {
    const SecretSchema schema = makeSchema();
    GError* error = nullptr;
    gchar* rawSecret = secret_password_lookup_sync(&schema, nullptr, &error, "service", service.c_str(), "username", user.c_str(), nullptr);
    const std::unique_ptr<gchar, decltype(&secret_password_free)> ownedSecret(rawSecret, &secret_password_free);
    if (error != nullptr || rawSecret == nullptr) {
        return std::unexpected(makeError(error));
    }
    return std::string(rawSecret);
}

errors::Error remove(const std::string& service, const std::string& user) {
    const SecretSchema schema = makeSchema();
    GError* error = nullptr;
    const gboolean deleted = secret_password_clear_sync(&schema, nullptr, &error, "service", service.c_str(), "username", user.c_str(), nullptr);
    if (error != nullptr || deleted == 0) {
        return makeError(error);
    }
    return {};
}

std::expected<bool, errors::Error> isAvailable() {
    GError* error = nullptr;
    SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
    if (error != nullptr || service == nullptr) {
        if (service != nullptr) {
            g_object_unref(service);
        }
        errors::Error result = {
            .errorCode = ErrorCode::Unavailable,
            .errorMessage = "Secret Service unavailable",
        };
        if (error != nullptr) {
            result = makeError(error);
            result.errorCode = ErrorCode::Unavailable;
        }
        return std::unexpected(result);
    }
    g_object_unref(service);
    return true;
}

} // namespace amj::keyring

#endif // __linux__
