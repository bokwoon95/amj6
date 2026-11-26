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

UvTimerHandle::~UvTimerHandle() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        fail("libuv timer cleanup failed", std::source_location::current());
    }
}

UvTimerHandle::UvTimerHandle(UvTimerHandle&& other) noexcept {
    state = std::move(other.state);
}

std::error_code UvTimerHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv timer with outstanding borrowers", sourceLocation, state->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&state->uvTimerHandle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv timer closed outside its owner", sourceLocation);
    }
    std::ignore = state.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<State*>(handle->data);
    });
    return {};
}

std::expected<UvPollHandle, std::error_code> UvPollHandle::create(Borrowed<UvLoop> loop,
    uv_os_sock_t socket) noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>(State{
            .uvLoop = std::move(loop),
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

UvPollHandle::~UvPollHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv poll handle cleanup failed", std::source_location::current());
    }
}

UvPollHandle::UvPollHandle(UvPollHandle&& other) noexcept {
    state = std::move(other.state);
}

std::error_code UvPollHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv poll handle with outstanding borrowers", sourceLocation, state->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&state->uvPollHandle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv poll handle closed outside its owner", sourceLocation);
    }
    std::ignore = state.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<State*>(handle->data);
    });
    return {};
}

std::expected<UvAsyncHandle, std::error_code> UvAsyncHandle::create(Borrowed<UvLoop> loop,
    uv_async_cb callback) noexcept {
    try {
        std::unique_ptr<State> state = std::make_unique<State>(State{
            .loop = std::move(loop),
        });
        const int status = uv_async_init(state->loop.get(), &state->handle, callback);
        if (status != 0) {
            return std::unexpected(std::error_code(status, uvErrorCategory));
        }
        state->handle.data = state.get();
        UvAsyncHandle uvAsyncHandle;
        uvAsyncHandle.state = std::move(state);
        return uvAsyncHandle;
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

UvAsyncHandle::~UvAsyncHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv async handle cleanup failed", std::source_location::current());
    }
}

UvAsyncHandle::UvAsyncHandle(UvAsyncHandle&& other) noexcept {
    state = std::move(other.state);
}

std::error_code UvAsyncHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (state->head.next != &state->head) {
        fail("closing a libuv async handle with outstanding borrowers", sourceLocation,
            state->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&state->handle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv async handle closed outside its owner", sourceLocation);
    }
    std::ignore = state.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<State*>(handle->data);
    });
    return {};
}

} // namespace amj::safe
