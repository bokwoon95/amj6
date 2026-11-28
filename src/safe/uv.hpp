#pragma once

#include "safe.hpp"

#include <uv.h>
#if defined(_WIN32)
#include <uv/win.h>
#else
#include <uv/unix.h>
#endif

#include <expected>
#include <memory>
#include <source_location>
#include <system_error>
#include <utility>

namespace amj::safe {

const std::error_category& uvErrorCategory() noexcept;

class UvLoop {
public:
    [[nodiscard]] static std::expected<UvLoop, std::error_code> create() noexcept;

    ~UvLoop() noexcept;
    UvLoop(const UvLoop&) = delete;
    UvLoop& operator=(const UvLoop&) = delete;
    UvLoop(UvLoop&& other) noexcept : state(std::move(other.state)) {}
    UvLoop& operator=(UvLoop&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_loop_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvLoop>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    UvLoop() noexcept = default;

    struct State {
        uv_loop_t uvLoop = {};
        Borrower borrowers = {};
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
    UvTimerHandle(UvTimerHandle&& other) noexcept : state(std::move(other.state)) {}
    UvTimerHandle& operator=(UvTimerHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_timer_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvTimerHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    UvTimerHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> uvLoop = {};
        uv_timer_t uvTimerHandle = {};
        Borrower borrowers = {};
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
    UvPollHandle(UvPollHandle&& other) noexcept : state(std::move(other.state)) {}
    UvPollHandle& operator=(UvPollHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_poll_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvPollHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    UvPollHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> uvLoop = {};
        uv_poll_t uvPollHandle = {};
        Borrower borrowers = {};
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
    UvAsyncHandle(UvAsyncHandle&& other) noexcept : state(std::move(other.state)) {}
    UvAsyncHandle& operator=(UvAsyncHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_async_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvAsyncHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    UvAsyncHandle() noexcept = default;

    struct State {
        Borrowed<UvLoop> uvLoop = {};
        uv_async_t uvAsyncHandle = {};
        Borrower borrowers = {};
    };
    std::unique_ptr<State> state = nullptr;
};

} // namespace amj::safe
