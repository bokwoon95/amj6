#include "uv.hpp"

#include "safe.hpp"

#include <uv.h>
#if defined(_WIN32)
#include <uv/win.h>
#else
#include <uv/unix.h>
#endif

#include <array>
#include <exception>
#include <expected>
#include <iostream>
#include <memory>
#include <new>
#include <source_location>
#include <string>
#include <system_error>
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

UvLoop::~UvLoop() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": UvLoop.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code UvLoop::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (!state->borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed UvLoop with outstanding borrowers:\n"}));
        for (const Borrower& borrower : state->borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    const int status = uv_loop_close(&state->uvLoop);
    if (status != 0) {
        return std::error_code(status, uvErrorCategory);
    }
    state.reset();
    return {};
}

uv_loop_t* UvLoop::get(std::source_location sourceLocation) const noexcept {
    if (state == nullptr) {
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, ": UvLoop is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return &state->uvLoop;
}

Borrowed<UvLoop> UvLoop::borrow(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        print(std::cerr,
            std::to_array<PrintArg>({sourceLocation, ": UvLoop is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<UvLoop>(sourceLocation, state->borrowers, &state->uvLoop);
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
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": UvTimerHandler.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code UvTimerHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (!state->borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed UvTimerHandle with outstanding borrowers:\n"}));
        for (const Borrower& borrower : state->borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&state.release()->uvTimerHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

uv_timer_t* UvTimerHandle::get(std::source_location sourceLocation) const noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvTimerHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return &state->uvTimerHandle;
}

Borrowed<UvTimerHandle> UvTimerHandle::borrow(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvTimerHandle is empty or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<UvTimerHandle>(sourceLocation, state->borrowers, &state->uvTimerHandle);
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

UvPollHandle::~UvPollHandle() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": UvPollHandle.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code UvPollHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (!state->borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed UvPollHandle with outstanding borrowers:\n"}));
        for (const Borrower& borrower : state->borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&state.release()->uvPollHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

uv_poll_t* UvPollHandle::get(std::source_location sourceLocation) const noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvPollHandle is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return &state->uvPollHandle;
}

Borrowed<UvPollHandle> UvPollHandle::borrow(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvPollHandle is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<UvPollHandle>(sourceLocation, state->borrowers, &state->uvPollHandle);
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

UvAsyncHandle::~UvAsyncHandle() noexcept {
    const std::error_code errorCode = close();
    if (errorCode) {
        print(std::cerr, std::to_array<PrintArg>({std::source_location::current(),
                             ": UvAsyncHandle.close: ", errorCode, "\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
}

std::error_code UvAsyncHandle::close(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        return {};
    }
    if (!state->borrowers.empty()) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": closed UvAsyncHandle with outstanding borrowers:\n"}));
        for (const Borrower& borrower : state->borrowers) {
            print(std::cerr, std::to_array<PrintArg>({"- ", borrower.sourceLocation, "\n"}));
        }
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    uv_close(reinterpret_cast<uv_handle_t*>(&state.release()->uvAsyncHandle),
        [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<State*>(uvHandle->data);
        });
    return {};
}

uv_async_t* UvAsyncHandle::get(std::source_location sourceLocation) const noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvAsyncHandle is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return &state->uvAsyncHandle;
}

Borrowed<UvAsyncHandle> UvAsyncHandle::borrow(std::source_location sourceLocation) noexcept {
    if (state == nullptr) {
        print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                             ": UvAsyncHandle is closed or moved-from\n"}));
        printStackTrace(std::cerr, 1, 30);
        std::terminate();
    }
    return Borrowed<UvAsyncHandle>(sourceLocation, state->borrowers, &state->uvAsyncHandle);
}

} // namespace amj::safe
