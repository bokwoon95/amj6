#pragma once

#include "safe.hpp"

#include <uv.h>

#include <expected>
#include <memory>
#include <source_location>
#include <system_error>

namespace amj::safe {

std::error_code uvError(int status) noexcept;

class UvLoop {
public:
    UvLoop();

    ~UvLoop() noexcept;
    UvLoop(const UvLoop&) = delete;
    UvLoop& operator=(const UvLoop&) = delete;
    UvLoop(UvLoop&& other) noexcept;
    UvLoop& operator=(UvLoop&&) = delete;

    [[nodiscard]] uv_loop_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvLoop>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        return Borrowed<UvLoop>::makeBorrowed(this->get(sourceLocation), this->head,
            sourceLocation);
    }

    // On failure the loop remains owned and usable. Drain pending close callbacks
    // before closure; destroying a loop that cannot close is fatal.
    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    std::unique_ptr<uv_loop_t> ownedLoop = nullptr;
    Borrower head = {};
};

// Each handle owns stable heap state retained until its close callback. The C
// handle's data field is reserved for this state. Successful close() schedules
// closure; it does not wait for completion. The loop borrow is released last,
// after handle storage and the empty borrower sentinel are destroyed.
class UvTimerHandle {
public:
    explicit UvTimerHandle(Borrowed<UvLoop> loop);
    ~UvTimerHandle() noexcept;
    UvTimerHandle(const UvTimerHandle&) = delete;
    UvTimerHandle& operator=(const UvTimerHandle&) = delete;
    UvTimerHandle(UvTimerHandle&& other) noexcept;
    UvTimerHandle& operator=(UvTimerHandle&&) = delete;

    [[nodiscard]] uv_timer_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;

    [[nodiscard]] Borrowed<UvTimerHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        uv_timer_t* handle = this->get(sourceLocation);
        return Borrowed<UvTimerHandle>::makeBorrowed(handle, this->ownedState->head,
            sourceLocation);
    }

    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    struct UvTimerState {
        Borrowed<UvLoop> loop = {};
        uv_timer_t handle = {};
        Borrower head = {};
    };
    std::unique_ptr<UvTimerState> ownedState = nullptr;
};

class UvPollHandle {
public:
    UvPollHandle(Borrowed<UvLoop> loop, uv_os_sock_t socket);
    ~UvPollHandle() noexcept;
    UvPollHandle(const UvPollHandle&) = delete;
    UvPollHandle& operator=(const UvPollHandle&) = delete;
    UvPollHandle(UvPollHandle&& other) noexcept;
    UvPollHandle& operator=(UvPollHandle&&) = delete;

    [[nodiscard]] uv_poll_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;
    [[nodiscard]] Borrowed<UvPollHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        uv_poll_t* handle = this->get(sourceLocation);
        return Borrowed<UvPollHandle>::makeBorrowed(handle, this->ownedState->head, sourceLocation);
    }
    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    struct UvPollState {
        Borrowed<UvLoop> loop = {};
        uv_poll_t handle = {};
        Borrower head = {};
    };
    std::unique_ptr<UvPollState> ownedState = nullptr;
};

class UvAsyncHandle {
public:
    UvAsyncHandle(Borrowed<UvLoop> loop, uv_async_cb callback);
    ~UvAsyncHandle() noexcept;
    UvAsyncHandle(const UvAsyncHandle&) = delete;
    UvAsyncHandle& operator=(const UvAsyncHandle&) = delete;
    UvAsyncHandle(UvAsyncHandle&& other) noexcept;
    UvAsyncHandle& operator=(UvAsyncHandle&&) = delete;

    [[nodiscard]] uv_async_t*
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept;
    [[nodiscard]] Borrowed<UvAsyncHandle>
    borrow(std::source_location sourceLocation = std::source_location::current()) noexcept {
        uv_async_t* handle = this->get(sourceLocation);
        return Borrowed<UvAsyncHandle>::makeBorrowed(handle, this->ownedState->head,
            sourceLocation);
    }
    [[nodiscard]] std::error_code
    close(std::source_location sourceLocation = std::source_location::current()) noexcept;

private:
    struct UvAsyncState {
        Borrowed<UvLoop> loop = {};
        uv_async_t handle = {};
        Borrower head = {};
    };
    std::unique_ptr<UvAsyncState> ownedState = nullptr;
};

std::expected<UvTimerHandle, std::error_code> createUvTimer(Borrowed<UvLoop> loop);
std::expected<UvPollHandle, std::error_code> createUvPoll(Borrowed<UvLoop> loop,
    uv_os_sock_t socket);
std::expected<UvAsyncHandle, std::error_code> createUvAsync(Borrowed<UvLoop> loop,
    uv_async_cb callback);

} // namespace amj::safe
