#pragma once

#include "safe.hpp"

#include <curl/curl.h>

#include <curl/multi.h>
#include <expected>
#include <system_error>

namespace amj::safe {

std::error_code curlMultiError(CURLMcode code) noexcept;

// Callers must initialize libcurl globally and unregister easy handles before
// closing them. Multi closure requires all easy handles to be removed first.
std::expected<Owned<CURL*>, std::error_code> createCurlEasy();
std::expected<Owned<CURLM*>, std::error_code> createCurlMulti();

} // namespace amj::safe
