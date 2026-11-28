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
class CurlErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "curl";
    }
    std::string message(int code) const override {
        return curl_easy_strerror(static_cast<CURLcode>(code));
    }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const CurlErrorCategory curlErrorCategoryInstance;

class CurlMErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "curlm";
    }
    std::string message(int code) const override {
        return curl_multi_strerror(static_cast<CURLMcode>(code));
    }
};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const CurlMErrorCategory curlMErrorCategoryInstance;
} // namespace

const std::error_category& curlErrorCategory() noexcept {
    return curlErrorCategoryInstance;
}

const std::error_category& curlMErrorCategory() noexcept {
    return curlMErrorCategoryInstance;
}

std::expected<CurlEasyHandle, std::error_code> CurlEasyHandle::create() noexcept {
    CurlEasyHandle curlEasyHandle;
    curlEasyHandle.ownedHandle = curl_easy_init();
    if (curlEasyHandle.ownedHandle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return curlEasyHandle;
}

CurlEasyHandle::~CurlEasyHandle() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": CurlEasyHandle.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code CurlEasyHandle::close(std::source_location sourceLocation) noexcept {
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed CurlEasyHandle with outstanding borrowers:\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
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

CURL* CurlEasyHandle::get(std::source_location sourceLocation) const noexcept {
    if (ownedHandle == nullptr) {
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, ": CurlEasyHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return ownedHandle;
}

Borrowed<CurlEasyHandle> CurlEasyHandle::borrow(std::source_location sourceLocation) noexcept {
    if (ownedHandle == nullptr) {
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, ": CurlEasyHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<CurlEasyHandle>(sourceLocation, borrowers, ownedHandle);
}

std::expected<CurlMultiHandle, std::error_code> CurlMultiHandle::create() noexcept {
    CurlMultiHandle curlMultiHandle;
    curlMultiHandle.ownedHandle = curl_multi_init();
    if (curlMultiHandle.ownedHandle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return curlMultiHandle;
}

CurlMultiHandle::~CurlMultiHandle() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": CurlMultiHandle.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code CurlMultiHandle::close(std::source_location sourceLocation) noexcept {
    if (!borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed CurlMultiHandle with outstanding borrowers:\n"}));
        for (const Borrower& borrower : borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.getSourceLocation(), "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    if (ownedHandle == nullptr) {
        return {};
    }
    const CURLMcode code = curl_multi_cleanup(ownedHandle);
    if (code != CURLM_OK) {
        return std::error_code(static_cast<int>(code), curlMErrorCategory());
    }
    ownedHandle = nullptr;
    return {};
}

CURLM* CurlMultiHandle::get(std::source_location sourceLocation) const noexcept {
    if (ownedHandle == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": CurlMultiHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return ownedHandle;
}

Borrowed<CurlMultiHandle> CurlMultiHandle::borrow(std::source_location sourceLocation) noexcept {
    if (ownedHandle == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": CurlMultiHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<CurlMultiHandle>(sourceLocation, borrowers, ownedHandle);
}

} // namespace amj::safe
