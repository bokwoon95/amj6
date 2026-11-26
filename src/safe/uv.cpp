#include "uv.hpp"

#include "safe.hpp"

#include <uv.h>

#include <expected>
#include <memory>
#include <new>
#include <source_location>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace amj::safe {
namespace {
class UvErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "libuv";
    }
    std::string message(int status) const override {
        return uv_strerror(status);
    }
};

const UvErrorCategory uvErrorCategory; // NOLINT(bugprone-throwing-static-initialization)
} // namespace

std::expected<UvLoop, std::error_code> UvLoop::create() noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>();
        const int status = uv_loop_init(&state->uvLoop);
        if (status != 0) {
            return std::unexpected(std::error_code(status, uvErrorCategory));
        }
        UvLoop uvLoop;
        uvLoop.state = std::move(state);
        return uvLoop;
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::error_code UvLoop::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv loop with outstanding borrowers", sourceLocation, state->head);
    }
    const int status = uv_loop_close(&state->uvLoop);
    if (status != 0) {
        return std::error_code(status, uvErrorCategory);
    }
    state.reset();
    return {};
}

std::expected<UvTimerHandle, std::error_code>
UvTimerHandle::create(Borrowed<UvLoop> uvLoop) noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>(State{
            .uvLoop = std::move(uvLoop),
        });
        const int status = uv_timer_init(state->uvLoop.get(), &state->uvTimerHandle);
        if (status != 0) {
            return std::unexpected(std::error_code(status, uvErrorCategory));
        }
        state->uvTimerHandle.data = state.get();
        UvTimerHandle uvTimerHandle;
        uvTimerHandle.state = std::move(state);
        return uvTimerHandle;
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::error_code UvTimerHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv timer with outstanding borrowers", sourceLocation, state->head);
    }
    std::ignore = state.release();
    uv_close(reinterpret_cast<uv_handle_t*>(&state->uvTimerHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

std::expected<UvPollHandle, std::error_code> UvPollHandle::create(Borrowed<UvLoop> uvLoop,
    uv_os_sock_t socket) noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>(State{
            .uvLoop = std::move(uvLoop),
        });
        const int status = uv_poll_init_socket(state->uvLoop.get(), &state->uvPollHandle, socket);
        if (status != 0) {
            return std::unexpected(std::error_code(status, uvErrorCategory));
        }
        state->uvPollHandle.data = state.get();
        UvPollHandle uvPollHandle;
        uvPollHandle.state = std::move(state);
        return uvPollHandle;
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::error_code UvPollHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv poll handle with outstanding borrowers", sourceLocation, state->head);
    }
    std::ignore = state.release();
    uv_close(reinterpret_cast<uv_handle_t*>(&state->uvPollHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

std::expected<UvAsyncHandle, std::error_code> UvAsyncHandle::create(Borrowed<UvLoop> uvLoop,
    uv_async_cb callback) noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>(State{
            .uvLoop = std::move(uvLoop),
        });
        const int status = uv_async_init(state->uvLoop.get(), &state->uvAsyncHandle, callback);
        if (status != 0) {
            return std::unexpected(std::error_code(status, uvErrorCategory));
        }
        state->uvAsyncHandle.data = state.get();
        UvAsyncHandle uvAsyncHandle;
        uvAsyncHandle.state = std::move(state);
        return uvAsyncHandle;
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::error_code UvAsyncHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv async handle with outstanding borrowers", sourceLocation,
            state->head);
    }
    std::ignore = state.release();
    uv_close(reinterpret_cast<uv_handle_t*>(&state->uvAsyncHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

} // namespace amj::safe
