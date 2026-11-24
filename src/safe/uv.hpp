#pragma once

#include "safe.hpp"

#include <uv.h>

#include <expected>
#include <system_error>

namespace amj::safe {

std::error_code uvError(int status) noexcept;

// Factories return initialized handles with null data. If data is later set,
// it must point to an owned VoidPtr, deleted by the final close callback.
// Closure schedules deletion: the loop must remain alive and be drained.
std::expected<Owned<uv_timer_t*>, std::error_code>
createUvTimer(const Borrowed<uv_loop_t*>& uvLoop);
std::expected<Owned<uv_async_t*>, std::error_code> createUvAsync(const Borrowed<uv_loop_t*>& uvLoop,
    uv_async_cb callback);
std::expected<Owned<uv_poll_t*>, std::error_code> createUvPoll(const Borrowed<uv_loop_t*>& uvLoop,
    uv_os_sock_t socket);

} // namespace amj::safe
