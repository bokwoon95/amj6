#include "curl.hpp"

#include "safe.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include <array>
#include <exception>
#include <expected>
#include <iostream>
#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace amj::safe {
namespace {
class CurlMultiCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "curl.multi";
    }
    std::string message(int code) const override {
        return curl_multi_strerror(static_cast<CURLMcode>(code));
    }
};
const CurlMultiCategory curlMultiCategory; // NOLINT(bugprone-throwing-static-initialization)
} // namespace

std::error_code curlMultiError(CURLMcode code) noexcept {
    return {static_cast<int>(code), curlMultiCategory};
}

CurlEasyHandle::CurlEasyHandle(CURL* ownedHandle) noexcept : ownedHandle(ownedHandle) {
    if (ownedHandle == nullptr) {
        const std::source_location sourceLocation = std::source_location::current();
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, " Cannot adopt a null curl easy handle\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

CurlEasyHandle::~CurlEasyHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        const std::source_location sourceLocation = std::source_location::current();
        print(std::cerr, std::to_array<PrintArg>({sourceLocation, " Curl easy cleanup failed\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

CurlEasyHandle::CurlEasyHandle(CurlEasyHandle&& other) noexcept
    : ownedHandle(std::exchange(other.ownedHandle, nullptr)), head(std::move(other.head)) {}

std::error_code CurlEasyHandle::close(std::source_location sourceLocation) noexcept {
    if (!head.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Closing a curl easy handle with outstanding borrowers\n"}));
        for (const Borrower& borrower : head) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    CURL* handle = std::exchange(ownedHandle, nullptr);
    if (handle != nullptr) {
        curl_easy_cleanup(handle);
    }
    return {};
}

CurlMultiHandle::CurlMultiHandle(CURLM* ownedHandle) noexcept : ownedHandle(ownedHandle) {
    if (ownedHandle == nullptr) {
        const std::source_location sourceLocation = std::source_location::current();
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, " Cannot adopt a null curl multi handle\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

CurlMultiHandle::~CurlMultiHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        const std::source_location sourceLocation = std::source_location::current();
        print(std::cerr, std::to_array<PrintArg>({sourceLocation, " Curl multi cleanup failed\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

CurlMultiHandle::CurlMultiHandle(CurlMultiHandle&& other) noexcept
    : ownedHandle(std::exchange(other.ownedHandle, nullptr)), head(std::move(other.head)) {}

std::error_code CurlMultiHandle::close(std::source_location sourceLocation) noexcept {
    if (!head.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             " Closing a curl multi handle with outstanding borrowers\n"}));
        for (const Borrower& borrower : head) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    CURLM* handle = std::exchange(ownedHandle, nullptr);
    if (handle == nullptr) {
        return {};
    }
    const CURLMcode code = curl_multi_cleanup(handle);
    if (code != CURLM_OK) {
        ownedHandle = handle;
        return curlMultiError(code);
    }
    return {};
}

std::expected<CurlEasyHandle, std::error_code> createCurlEasy() {
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return CurlEasyHandle(handle);
}

std::expected<CurlMultiHandle, std::error_code> createCurlMulti() {
    CURLM* handle = curl_multi_init();
    if (handle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return CurlMultiHandle(handle);
}

} // namespace amj::safe
