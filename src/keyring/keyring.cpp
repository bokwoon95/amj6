#include "keyring.hpp"

#include <string>
#include <system_error>

namespace amj::keyring {

namespace {

class ErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "amj.keyring";
    }

    std::string message(int value) const override {
        switch (value) {
            case static_cast<int>(ErrorCode::NotFound):
                return "Secret not found";
            case static_cast<int>(ErrorCode::Unavailable):
                return "Credential store unavailable";
            case static_cast<int>(ErrorCode::SecretTooLong):
                return "Secret exceeds credential store capacity";
            case static_cast<int>(ErrorCode::AccessDenied):
                return "Credential store access denied";
            default:
                return "Unknown keyring error";
        }
    }
};

} // namespace

std::error_code make_error_code(ErrorCode errorCode) {
    static const ErrorCategory category;
    return std::error_code(static_cast<int>(errorCode), category);
}

} // namespace amj::keyring
