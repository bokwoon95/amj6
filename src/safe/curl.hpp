#pragma once

#include "safe.hpp"

#include <curl/curl.h>

#include <curl/multi.h>
#include <expected>
#include <source_location>
#include <system_error>
#include <utility>

namespace amj::safe {

const std::error_category& curlErrorCategory() noexcept;

const std::error_category& curlMErrorCategory() noexcept;

// libcurl global initialization is the caller's responsibility. Close attached
// easy handles before closing their multi handle. Do not close handles inside
// libcurl callbacks or manually change associations managed by these wrappers.
class CurlMultiHandle {
public:
    [[nodiscard]] static std::expected<CurlMultiHandle, std::error_code> create() noexcept;

    ~CurlMultiHandle() noexcept;
    CurlMultiHandle(const CurlMultiHandle&) = delete;
    CurlMultiHandle& operator=(const CurlMultiHandle&) = delete;
    CurlMultiHandle(CurlMultiHandle&& other) noexcept
        : ownedCurlM(std::exchange(other.ownedCurlM, nullptr)),
          borrowers(std::move(other.borrowers)) {}
    CurlMultiHandle& operator=(CurlMultiHandle&&) = delete;

    // Failed cleanup retains ownership; destruction with an unhandled error is fatal.
    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] CURLM*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<CurlMultiHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    CurlMultiHandle() noexcept = default;

    CURLM* ownedCurlM = nullptr;
    Borrower borrowers = {};
};

class CurlEasyHandle {
public:
    [[nodiscard]] static std::expected<CurlEasyHandle, std::error_code> create() noexcept;

    [[nodiscard]] static std::expected<CurlEasyHandle, std::error_code>
    createFromMulti(Borrowed<CurlMultiHandle> curlMultiHandle) noexcept;

    ~CurlEasyHandle() noexcept;
    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;
    CurlEasyHandle(CurlEasyHandle&& other) noexcept
        : curlMultiHandle(std::move(other.curlMultiHandle)),
          ownedCurl(std::exchange(other.ownedCurl, nullptr)),
          borrowers(std::move(other.borrowers)) {}
    CurlEasyHandle& operator=(CurlEasyHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] CURL*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<CurlEasyHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    CurlEasyHandle() noexcept = default;

    Borrowed<CurlMultiHandle> curlMultiHandle = {};
    CURL* ownedCurl = nullptr;
    Borrower borrowers = {};
};

} // namespace amj::safe
