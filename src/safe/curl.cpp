#include "curl.hpp"

#include "safe.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include <expected>
#include <string>
#include <system_error>

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

std::expected<Owned<CURL*>, std::error_code> createCurlEasy() {
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return Owned<CURL*>(handle, [](CURL* handle) noexcept -> std::error_code {
        curl_easy_cleanup(handle);
        return {};
    });
}

std::expected<Owned<CURLM*>, std::error_code> createCurlMulti() {
    CURLM* handle = curl_multi_init();
    if (handle == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    return Owned<CURLM*>(handle, [](CURLM* handle) noexcept -> std::error_code {
        const CURLMcode code = curl_multi_cleanup(handle);
        if (code != CURLM_OK) {
            // A failed multi teardown is outside the consumed-resource contract.
            fail("curl multi cleanup failed", nullptr);
        }
        return curlMultiError(code);
    });
}

} // namespace amj::safe
