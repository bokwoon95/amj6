#include "keyring.hpp"

#include "errors/errors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace amj::keyring {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

TEST(KeyringErrors, CodesHaveStableCategoryAndMessages) {
    const std::array<ErrorCode, 4> errorCodes = {ErrorCode::NotFound, ErrorCode::Unavailable, ErrorCode::SecretTooLong, ErrorCode::AccessDenied};
    const std::array<std::string, 4> messages = {"Secret not found", "Credential store unavailable", "Secret exceeds credential store capacity", "Credential store access denied"};
    for (std::size_t index = 0; index < errorCodes.size(); ++index) {
        const std::error_code errorCode = make_error_code(errorCodes.at(index));
        EXPECT_STREQ(errorCode.category().name(), "amj.keyring");
        EXPECT_EQ(errorCode.value(), static_cast<int>(errorCodes.at(index)));
        EXPECT_EQ(errorCode.message(), messages.at(index));
        EXPECT_EQ(&errorCode.category(), &make_error_code(ErrorCode::NotFound).category());
        const errors::Error error = {
            .errorCode = errorCode,
        };
        EXPECT_TRUE(error.is(errorCodes.at(index)));
        EXPECT_NE(errorCode, std::make_error_code(std::errc::permission_denied));
    }
    EXPECT_EQ(make_error_code(ErrorCode::NotFound).category().message(255), "Unknown keyring error");
}

class KeyringIntegration : public testing::Test {
  protected:
    std::string service = "";
    std::vector<std::string> services = {};
    const std::array<std::string, 2> users = {"test-user", "test-user-\xE7\x94\xA8\xE6\x88\xB7"};

    void SetUp() override {
        std::random_device randomDevice;
        service = "amj.keyring.tests." + std::to_string(chrono::system_clock::now().time_since_epoch().count()) + "." + std::to_string(randomDevice()) + "." + std::to_string(randomDevice());
        std::cerr << "Keyring integration test: OS authorization may be required; waiting for native calls. Service: " << service << '\n';
        const std::expected<bool, errors::Error> availableResult = isAvailable();
        if (!availableResult.has_value()) {
            if (availableResult.error().is(ErrorCode::AccessDenied) || availableResult.error().is(ErrorCode::Unavailable)) {
                GTEST_SKIP() << "isAvailable: " << availableResult.error().toString();
            }
            FAIL() << "isAvailable: " << availableResult.error().toString();
        }
        if (!availableResult.value()) {
            GTEST_SKIP() << "OS credential store is unavailable.";
        }
        // Register identities before writes so cleanup also runs after assertion failures.
        services = {service, service + ".\xE6\x9C\x8D\xE5\x8A\xA1"};
    }

    void TearDown() override {
        for (const std::string& entryService : services) {
            for (const std::string& user : users) {
                const errors::Error removeError = remove(entryService, user);
                if (!removeError.errorCode || removeError.is(ErrorCode::NotFound)) {
                    continue;
                }
                std::cerr << "Keyring cleanup failed; remove this test entry manually: service=" << entryService << ", user=" << user << ": " << removeError.toString() << '\n';
                if (!removeError.is(ErrorCode::AccessDenied) && !removeError.is(ErrorCode::Unavailable)) {
                    ADD_FAILURE() << "Unexpected keyring cleanup error: " << removeError.toString();
                }
            }
        }
    }
};

TEST_F(KeyringIntegration, MissingEntriesReportNotFound) {
    const std::expected<std::string, errors::Error> loadResult = load(service, users.at(0));
    if (!loadResult.has_value() && (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << loadResult.error().toString();
    }
    ASSERT_FALSE(loadResult.has_value());
    EXPECT_TRUE(loadResult.error().is(ErrorCode::NotFound)) << loadResult.error().toString();
    const errors::Error removeError = remove(service, users.at(0));
    if (removeError.errorCode && (removeError.is(ErrorCode::AccessDenied) || removeError.is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << removeError.toString();
    }
    ASSERT_TRUE(static_cast<bool>(removeError.errorCode));
    EXPECT_TRUE(removeError.is(ErrorCode::NotFound)) << removeError.toString();
}

TEST_F(KeyringIntegration, RoundTripOverwriteUnicodeAndSizes) {
    // Exercise both sides of Windows' direct-blob limit, multiple encrypted
    // attributes, and large-to-small updates without assuming OS storage format.
    // Empty overwrites are outside application usage; delete entries instead.
    const std::vector<std::string> values = {"original", "replacement-\xE5\xAF\x86\xE7\xA0\x81", "short", std::string(2560, 'a'), std::string(2561, 'b'), std::string(12000, 'c'), "small-again", "final"};
    for (const std::string& value : values) {
        SCOPED_TRACE("Secret size: " + std::to_string(value.size()));
        const errors::Error saveError = save(service, users.at(0), value);
        if (saveError.errorCode) {
            if (saveError.is(ErrorCode::AccessDenied) || saveError.is(ErrorCode::Unavailable)) {
                GTEST_SKIP() << "save: " << saveError.toString();
            }
            FAIL() << "save: " << saveError.toString();
        }
        const std::expected<std::string, errors::Error> loadResult = load(service, users.at(0));
        if (!loadResult.has_value()) {
            if (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable)) {
                GTEST_SKIP() << "load: " << loadResult.error().toString();
            }
            FAIL() << "load: " << loadResult.error().toString();
        }
        // Do not print credential contents on mismatch.
        EXPECT_TRUE(loadResult.value() == value) << "Loaded secret differs from saved bytes";
    }
    const errors::Error removeError = remove(service, users.at(0));
    if (removeError.errorCode) {
        if (removeError.is(ErrorCode::AccessDenied) || removeError.is(ErrorCode::Unavailable)) {
            GTEST_SKIP() << "remove: " << removeError.toString();
        }
        FAIL() << "remove: " << removeError.toString();
    }
    const std::expected<std::string, errors::Error> loadResult = load(service, users.at(0));
    if (!loadResult.has_value() && (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << loadResult.error().toString();
    }
    ASSERT_FALSE(loadResult.has_value());
    EXPECT_TRUE(loadResult.error().is(ErrorCode::NotFound)) << loadResult.error().toString();
}

TEST_F(KeyringIntegration, ServicesAndUsersAreIndependent) {
    int valueIndex = 0;
    for (const std::string& entryService : services) {
        for (const std::string& user : users) {
            const errors::Error saveError = save(entryService, user, "value-" + std::to_string(valueIndex));
            if (saveError.errorCode) {
                if (saveError.is(ErrorCode::AccessDenied) || saveError.is(ErrorCode::Unavailable)) {
                    GTEST_SKIP() << "save: " << saveError.toString();
                }
                FAIL() << "save: " << saveError.toString();
            }
            ++valueIndex;
        }
    }
    const errors::Error overwriteError = save(service, users.at(0), "changed");
    if (overwriteError.errorCode) {
        if (overwriteError.is(ErrorCode::AccessDenied) || overwriteError.is(ErrorCode::Unavailable)) {
            GTEST_SKIP() << "overwrite: " << overwriteError.toString();
        }
        FAIL() << "overwrite: " << overwriteError.toString();
    }
    valueIndex = 0;
    for (const std::string& entryService : services) {
        for (const std::string& user : users) {
            const std::expected<std::string, errors::Error> loadResult = load(entryService, user);
            if (!loadResult.has_value()) {
                if (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable)) {
                    GTEST_SKIP() << "load: " << loadResult.error().toString();
                }
                FAIL() << "load: " << loadResult.error().toString();
            }
            std::string expectedValue = "value-" + std::to_string(valueIndex);
            if (valueIndex == 0) {
                expectedValue = "changed";
            }
            EXPECT_TRUE(loadResult.value() == expectedValue);
            ++valueIndex;
        }
    }
}

// Linux explicitly documents UTF-8, NUL-terminated text rather than binary secrets.
#if defined(_WIN32) || defined(__APPLE__)
TEST_F(KeyringIntegration, EmbeddedNulBytesRoundTrip) {
    const std::string value("before\0after\0", 13);
    const errors::Error saveError = save(service, users.at(0), value);
    if (saveError.errorCode) {
        if (saveError.is(ErrorCode::AccessDenied) || saveError.is(ErrorCode::Unavailable)) {
            GTEST_SKIP() << "save: " << saveError.toString();
        }
        FAIL() << "save: " << saveError.toString();
    }
    const std::expected<std::string, errors::Error> loadResult = load(service, users.at(0));
    if (!loadResult.has_value()) {
        if (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable)) {
            GTEST_SKIP() << "load: " << loadResult.error().toString();
        }
        FAIL() << "load: " << loadResult.error().toString();
    }
    EXPECT_TRUE(loadResult.value() == value);
}
#endif

#if defined(_WIN32)
TEST_F(KeyringIntegration, OversizedWritePreservesExistingSecret) {
    const errors::Error saveError = save(service, users.at(0), "original");
    if (saveError.errorCode) {
        if (saveError.is(ErrorCode::AccessDenied) || saveError.is(ErrorCode::Unavailable)) {
            GTEST_SKIP() << "save: " << saveError.toString();
        }
        FAIL() << "save: " << saveError.toString();
    }
    // DPAPI overhead means even plaintext at the ciphertext ceiling cannot fit.
    for (const std::size_t size : {18944U, 18945U}) {
        const errors::Error oversizedError = save(service, users.at(0), std::string(size, 'x'));
        ASSERT_TRUE(static_cast<bool>(oversizedError.errorCode));
        EXPECT_TRUE(oversizedError.is(ErrorCode::SecretTooLong)) << oversizedError.toString();
        const std::expected<std::string, errors::Error> loadResult = load(service, users.at(0));
        if (!loadResult.has_value()) {
            if (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable)) {
                GTEST_SKIP() << "load: " << loadResult.error().toString();
            }
            FAIL() << "load: " << loadResult.error().toString();
        }
        EXPECT_TRUE(loadResult.value() == "original");
    }
}
#endif

} // namespace
} // namespace amj::keyring
