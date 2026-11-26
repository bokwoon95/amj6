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
class UvCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "libuv";
    }
    std::string message(int status) const override {
        return uv_strerror(status);
    }
};
const UvCategory uvCategory; // NOLINT(bugprone-throwing-static-initialization)
} // namespace

std::error_code uvError(int status) noexcept {
    return {status, uvCategory};
}

UvLoop::UvLoop() {
    this->ownedLoop = std::make_unique<uv_loop_t>();
    const int status = uv_loop_init(this->ownedLoop.get());
    if (status != 0) {
        throw std::system_error(uvError(status));
    }
}

UvLoop::~UvLoop() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv loop cleanup failed; drain and close all handles first",
            std::source_location::current());
    }
}

UvLoop::UvLoop(UvLoop&& other) noexcept : head(std::move(other.head)) {
    this->ownedLoop = std::move(other.ownedLoop);
}

uv_loop_t* UvLoop::get(std::source_location location) const noexcept {
    if (ownedLoop == nullptr) {
        fail("access through a closed or moved libuv loop", location);
    }
    return ownedLoop.get();
}

Borrowed<UvLoop> UvLoop::borrow(std::source_location location) noexcept {
    return Borrowed<UvLoop>(get(location), head, location);
}

std::error_code UvLoop::close(std::source_location location) noexcept {
    if (head.next != &head) {
        fail("closing a libuv loop with outstanding borrowers", location, head);
    }
    if (ownedLoop == nullptr) {
        return {};
    }
    const int status = uv_loop_close(ownedLoop.get());
    if (status != 0) {
        return uvError(status);
    }
    ownedLoop.reset();
    return {};
}

UvTimerHandle::UvTimerHandle(Borrowed<UvLoop> loop) {
    this->ownedState = std::make_unique<UvTimerState>(UvTimerState{
        .loop = std::move(loop),
    });
    const int status = uv_timer_init(this->ownedState->loop.get(), &this->ownedState->handle);
    if (status != 0) {
        throw std::system_error(uvError(status));
    }
    this->ownedState->handle.data = this->ownedState.get();
}

UvTimerHandle::~UvTimerHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv timer cleanup failed", std::source_location::current());
    }
}

UvTimerHandle::UvTimerHandle(UvTimerHandle&& other) noexcept {
    this->ownedState = std::move(other.ownedState);
}

uv_timer_t* UvTimerHandle::get(std::source_location location) const noexcept {
    if (ownedState == nullptr) {
        fail("access through a closed or moved libuv timer", location);
    }
    return &ownedState->handle;
}

Borrowed<UvTimerHandle> UvTimerHandle::borrow(std::source_location location) noexcept {
    uv_timer_t* handle = get(location);
    return Borrowed<UvTimerHandle>(handle, ownedState->head, location);
}

std::error_code UvTimerHandle::close(std::source_location location) noexcept {
    if (ownedState == nullptr) {
        return {};
    }
    if (ownedState->head.next != &ownedState->head) {
        fail("closing a libuv timer with outstanding borrowers", location, ownedState->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&ownedState->handle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv timer closed outside its owner", location);
    }
    std::ignore = ownedState.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<UvTimerState*>(handle->data);
    });
    return {};
}

UvPollHandle::UvPollHandle(Borrowed<UvLoop> loop, uv_os_sock_t socket) {
    this->ownedState = std::make_unique<UvPollState>(UvPollState{
        .loop = std::move(loop),
    });
    const int status =
        uv_poll_init_socket(this->ownedState->loop.get(), &this->ownedState->handle, socket);
    if (status != 0) {
        throw std::system_error(uvError(status));
    }
    this->ownedState->handle.data = this->ownedState.get();
}

UvPollHandle::~UvPollHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv poll handle cleanup failed", std::source_location::current());
    }
}

UvPollHandle::UvPollHandle(UvPollHandle&& other) noexcept {
    this->ownedState = std::move(other.ownedState);
}

uv_poll_t* UvPollHandle::get(std::source_location location) const noexcept {
    if (ownedState == nullptr) {
        fail("access through a closed or moved libuv poll handle", location);
    }
    return &ownedState->handle;
}

Borrowed<UvPollHandle> UvPollHandle::borrow(std::source_location location) noexcept {
    uv_poll_t* handle = get(location);
    return Borrowed<UvPollHandle>(handle, ownedState->head, location);
}

std::error_code UvPollHandle::close(std::source_location location) noexcept {
    if (ownedState == nullptr) {
        return {};
    }
    if (ownedState->head.next != &ownedState->head) {
        fail("closing a libuv poll handle with outstanding borrowers", location, ownedState->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&ownedState->handle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv poll handle closed outside its owner", location);
    }
    std::ignore = ownedState.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<UvPollState*>(handle->data);
    });
    return {};
}

UvAsyncHandle::UvAsyncHandle(Borrowed<UvLoop> loop, uv_async_cb callback) {
    this->ownedState = std::make_unique<UvAsyncState>(UvAsyncState{
        .loop = std::move(loop),
    });
    const int status =
        uv_async_init(this->ownedState->loop.get(), &this->ownedState->handle, callback);
    if (status != 0) {
        throw std::system_error(uvError(status));
    }
    this->ownedState->handle.data = this->ownedState.get();
}

UvAsyncHandle::~UvAsyncHandle() noexcept {
    const std::error_code error = close();
    if (error) {
        fail("libuv async handle cleanup failed", std::source_location::current());
    }
}

UvAsyncHandle::UvAsyncHandle(UvAsyncHandle&& other) noexcept {
    this->ownedState = std::move(other.ownedState);
}

uv_async_t* UvAsyncHandle::get(std::source_location location) const noexcept {
    if (ownedState == nullptr) {
        fail("access through a closed or moved libuv async handle", location);
    }
    return &ownedState->handle;
}

Borrowed<UvAsyncHandle> UvAsyncHandle::borrow(std::source_location location) noexcept {
    uv_async_t* handle = get(location);
    return Borrowed<UvAsyncHandle>(handle, ownedState->head, location);
}

std::error_code UvAsyncHandle::close(std::source_location location) noexcept {
    if (ownedState == nullptr) {
        return {};
    }
    if (ownedState->head.next != &ownedState->head) {
        fail("closing a libuv async handle with outstanding borrowers", location, ownedState->head);
    }
    // libuv requires conversion between its concrete handle and base C structs.
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&ownedState->handle);
    if (uv_is_closing(handle) != 0) {
        fail("libuv async handle closed outside its owner", location);
    }
    std::ignore = ownedState.release();
    uv_close(handle, [](uv_handle_t* handle) noexcept {
        delete static_cast<UvAsyncState*>(handle->data);
    });
    return {};
}

std::expected<UvTimerHandle, std::error_code> createUvTimer(Borrowed<UvLoop> loop) {
    try {
        return UvTimerHandle(std::move(loop));
    } catch (const std::system_error& error) {
        return std::unexpected(error.code());
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::expected<UvPollHandle, std::error_code> createUvPoll(Borrowed<UvLoop> loop,
    uv_os_sock_t socket) {
    try {
        return UvPollHandle(std::move(loop), socket);
    } catch (const std::system_error& error) {
        return std::unexpected(error.code());
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

std::expected<UvAsyncHandle, std::error_code> createUvAsync(Borrowed<UvLoop> loop,
    uv_async_cb callback) {
    try {
        return UvAsyncHandle(std::move(loop), callback);
    } catch (const std::system_error& error) {
        return std::unexpected(error.code());
    } catch (const std::bad_alloc&) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

} // namespace amj::safe
