#pragma once

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
#include <source_location>
#include <system_error>
#include <utility>

namespace amj::safe {

class UvLoop {
public:
    [[nodiscard]] static std::expected<UvLoop, std::error_code> create() noexcept;

    ~UvLoop() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation,
                    " Libuv loop cleanup failed; drain and close all handles first\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    };
    UvLoop(const UvLoop&) = delete;
    UvLoop& operator=(const UvLoop&) = delete;
    UvLoop(UvLoop&& other) noexcept : state(std::move(other.state)) {};
    UvLoop& operator=(UvLoop&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_loop_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through a closed or moved libuv loop\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return &state->uvLoop;
    }

    [[nodiscard]] Borrowed<UvLoop>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing a closed or moved libuv loop\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<UvLoop>(sourceLocation, state->borrowers, &state->uvLoop);
    }

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

    ~UvTimerHandle() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation, " Libuv timer cleanup failed\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    };
    UvTimerHandle(const UvTimerHandle&) = delete;
    UvTimerHandle& operator=(const UvTimerHandle&) = delete;
    UvTimerHandle(UvTimerHandle&& other) noexcept : state(std::move(other.state)) {};
    UvTimerHandle& operator=(UvTimerHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_timer_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through a closed or moved libuv timer\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return &state->uvTimerHandle;
    }

    [[nodiscard]] Borrowed<UvTimerHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing a closed or moved libuv timer\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<UvTimerHandle>(sourceLocation, state->borrowers, &state->uvTimerHandle);
    }

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

    ~UvPollHandle() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation, " Libuv poll handle cleanup failed\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    };
    UvPollHandle(const UvPollHandle&) = delete;
    UvPollHandle& operator=(const UvPollHandle&) = delete;
    UvPollHandle(UvPollHandle&& other) noexcept : state(std::move(other.state)) {};
    UvPollHandle& operator=(UvPollHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_poll_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through a closed or moved libuv poll handle\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return &state->uvPollHandle;
    }
    [[nodiscard]] Borrowed<UvPollHandle>

    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing a closed or moved libuv poll handle\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<UvPollHandle>(sourceLocation, state->borrowers, &state->uvPollHandle);
    }

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

    ~UvAsyncHandle() noexcept {
        const std::error_code errorCode = close();
        if (errorCode) {
            const std::source_location sourceLocation = std::source_location::current();
            print(std::cerr,
                std::to_array<PrintArg>({sourceLocation, " Libuv async handle cleanup failed\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
    };
    UvAsyncHandle(const UvAsyncHandle&) = delete;
    UvAsyncHandle& operator=(const UvAsyncHandle&) = delete;
    UvAsyncHandle(UvAsyncHandle&& other) noexcept : state(std::move(other.state)) {};
    UvAsyncHandle& operator=(UvAsyncHandle&&) = delete;

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

    [[nodiscard]] uv_async_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through a closed or moved libuv async handle\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return &state->uvAsyncHandle;
    }

    [[nodiscard]] Borrowed<UvAsyncHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        if (state == nullptr) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing a closed or moved libuv async handle\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<UvAsyncHandle>(sourceLocation, state->borrowers, &state->uvAsyncHandle);
    }

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
