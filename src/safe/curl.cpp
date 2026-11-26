#include "curl.hpp"

#include "safe.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include <expected>
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
        fail("cannot adopt a null curl easy handle", std::source_location::current());
    }
}

CurlEasyHandle::~CurlEasyHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("curl easy cleanup failed", std::source_location::current());
    }
}

CurlEasyHandle::CurlEasyHandle(CurlEasyHandle&& other) noexcept
    : ownedHandle(std::exchange(other.ownedHandle, nullptr)), head(std::move(other.head)) {}

CURL* CurlEasyHandle::get(std::source_location location) const noexcept {
    if (ownedHandle == nullptr) {
        fail("access through a closed or moved curl easy handle", location);
    }
    return ownedHandle;
}

Borrowed<CurlEasyHandle> CurlEasyHandle::borrow(std::source_location location) noexcept {
    return Borrowed<CurlEasyHandle>(get(location), head, location);
}

std::error_code CurlEasyHandle::close(std::source_location location) noexcept {
    if (head.next != &head) {
        fail("closing a curl easy handle with outstanding borrowers", location, head);
    }
    CURL* handle = std::exchange(ownedHandle, nullptr);
    if (handle != nullptr) {
        curl_easy_cleanup(handle);
    }
    return {};
}

CurlMultiHandle::CurlMultiHandle(CURLM* ownedHandle) noexcept : ownedHandle(ownedHandle) {
    if (ownedHandle == nullptr) {
        fail("cannot adopt a null curl multi handle", std::source_location::current());
    }
}

CurlMultiHandle::~CurlMultiHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("curl multi cleanup failed", std::source_location::current());
    }
}

CurlMultiHandle::CurlMultiHandle(CurlMultiHandle&& other) noexcept
    : ownedHandle(std::exchange(other.ownedHandle, nullptr)), head(std::move(other.head)) {}

CURLM* CurlMultiHandle::get(std::source_location location) const noexcept {
    if (ownedHandle == nullptr) {
        fail("access through a closed or moved curl multi handle", location);
    }
    return ownedHandle;
}

Borrowed<CurlMultiHandle> CurlMultiHandle::borrow(std::source_location location) noexcept {
    return Borrowed<CurlMultiHandle>(get(location), head, location);
}

std::error_code CurlMultiHandle::close(std::source_location location) noexcept {
    if (head.next != &head) {
        fail("closing a curl multi handle with outstanding borrowers", location, head);
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
