#pragma once

#include "safe.hpp"

#include <curl/curl.h>
#include <curl/multi.h>

#include <expected>
#include <source_location>
#include <system_error>

namespace amj::safe {

std::error_code curlMultiError(CURLMcode code) noexcept;

// libcurl global initialization is the caller's responsibility. Remove easy
// handles from their multi handle and finish active operations before cleanup.
class CurlEasyHandle {
public:
    explicit CurlEasyHandle(CURL* ownedHandle) noexcept;
    ~CurlEasyHandle() noexcept;
    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;
    CurlEasyHandle(CurlEasyHandle&& other) noexcept;
    CurlEasyHandle& operator=(CurlEasyHandle&&) = delete;

    [[nodiscard]] CURL*
    get(std::source_location location = std::source_location::current()) const noexcept;
    [[nodiscard]] Borrowed<CurlEasyHandle>
    borrow(std::source_location location = std::source_location::current()) noexcept;
    [[nodiscard]] std::error_code
    close(std::source_location location = std::source_location::current()) noexcept;

private:
    CURL* ownedHandle = nullptr;
    Borrower head = {};
};

class CurlMultiHandle {
public:
    explicit CurlMultiHandle(CURLM* ownedHandle) noexcept;
    ~CurlMultiHandle() noexcept;
    CurlMultiHandle(const CurlMultiHandle&) = delete;
    CurlMultiHandle& operator=(const CurlMultiHandle&) = delete;
    CurlMultiHandle(CurlMultiHandle&& other) noexcept;
    CurlMultiHandle& operator=(CurlMultiHandle&&) = delete;

    [[nodiscard]] CURLM*
    get(std::source_location location = std::source_location::current()) const noexcept;
    [[nodiscard]] Borrowed<CurlMultiHandle>
    borrow(std::source_location location = std::source_location::current()) noexcept;
    // Failed cleanup retains ownership; destruction with an unhandled error is fatal.
    [[nodiscard]] std::error_code
    close(std::source_location location = std::source_location::current()) noexcept;

private:
    CURLM* ownedHandle = nullptr;
    Borrower head = {};
};

std::expected<CurlEasyHandle, std::error_code> createCurlEasy();
std::expected<CurlMultiHandle, std::error_code> createCurlMulti();

} // namespace amj::safe
