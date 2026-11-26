#pragma once

#include "safe.hpp"

#include <utility>
#include <uv.h>

#include <expected>
#include <memory>
#include <source_location>
#include <system_error>

namespace amj::safe {

class UvLoop {
public:
    [[nodiscard]] static std::expected<UvLoop, std::error_code> create() noexcept;

    ~UvLoop() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            fail("libuv loop cleanup failed; drain and close all handles first",
                std::source_location::current());
        }
    };
    UvLoop(const UvLoop&) = delete;
    UvLoop& operator=(const UvLoop&) = delete;
    UvLoop(UvLoop&& other) noexcept {
        state = std::move(other.state);
    };
    UvLoop& operator=(UvLoop&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_loop_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            fail("access through a closed or moved libuv loop", sourceLocation);
        }
        return &state->uvLoop;
    }

    [[nodiscard]] Borrowed<UvLoop>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            fail("borrowing a closed or moved libuv loop", sourceLocation);
        }
        return Borrowed<UvLoop>(sourceLocation, state->head, &state->uvLoop);
    }

private:
    UvLoop() noexcept = default;

    struct State {
        uv_loop_t uvLoop = {};
        Borrower head = {};
    };
    std::unique_ptr<State> state = nullptr;
};

class UvTimerHandle {
public:
    [[nodiscard]] static std::expected<UvTimerHandle, std::error_code>
    create(Borrowed<UvLoop> uvLoop) noexcept;

    ~UvTimerHandle() noexcept;
    UvTimerHandle(const UvTimerHandle&) = delete;
    UvTimerHandle& operator=(const UvTimerHandle&) = delete;
    UvTimerHandle(UvTimerHandle&& other) noexcept;
    UvTimerHandle& operator=(UvTimerHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_timer_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            fail("access through a closed or moved libuv timer", sourceLocation);
        }
        return &state->uvTimerHandle;
    }

    [[nodiscard]] Borrowed<UvTimerHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            fail("borrowing a closed or moved libuv timer", sourceLocation);
        }
        return Borrowed<UvTimerHandle>(sourceLocation, state->head, &state->uvTimerHandle);
    }

private:
    UvTimerHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> uvLoop = {};
        uv_timer_t uvTimerHandle = {};
        Borrower head = {};
    };
    std::unique_ptr<State> state = nullptr;
};

class UvPollHandle {
public:
    [[nodiscard]] static std::expected<UvPollHandle, std::error_code> create(Borrowed<UvLoop> loop,
        uv_os_sock_t socket) noexcept;

    ~UvPollHandle() noexcept;
    UvPollHandle(const UvPollHandle&) = delete;
    UvPollHandle& operator=(const UvPollHandle&) = delete;
    UvPollHandle(UvPollHandle&& other) noexcept;
    UvPollHandle& operator=(UvPollHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_poll_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            fail("access through a closed or moved libuv poll handle", sourceLocation);
        }
        return &state->uvPollHandle;
    }
    [[nodiscard]] Borrowed<UvPollHandle>

    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            fail("borrowing a closed or moved libuv poll handle", sourceLocation);
        }
        return Borrowed<UvPollHandle>(sourceLocation, state->head, &state->uvPollHandle);
    }

private:
    UvPollHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> uvLoop = {};
        uv_poll_t uvPollHandle = {};
        Borrower head = {};
    };
    std::unique_ptr<State> state = nullptr;
};

class UvAsyncHandle {
public:
    [[nodiscard]] static std::expected<UvAsyncHandle, std::error_code>
    create(Borrowed<UvLoop> uvLoop, uv_async_cb uvAsyncCallback) noexcept;

    ~UvAsyncHandle() noexcept;
    UvAsyncHandle(const UvAsyncHandle&) = delete;
    UvAsyncHandle& operator=(const UvAsyncHandle&) = delete;
    UvAsyncHandle(UvAsyncHandle&& other) noexcept;
    UvAsyncHandle& operator=(UvAsyncHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_async_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            fail("access through a closed or moved libuv async handle", sourceLocation);
        }
        return &state->handle;
    }

    [[nodiscard]] Borrowed<UvAsyncHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            fail("borrowing a closed or moved libuv async handle", sourceLocation);
        }
        return Borrowed<UvAsyncHandle>(sourceLocation, state->head, &state->handle);
    }

private:
    UvAsyncHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> loop = {};
        uv_async_t handle = {};
        Borrower head = {};
    };
    std::unique_ptr<State> state = nullptr;
};

} // namespace amj::safe
