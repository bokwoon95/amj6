#pragma once

#include "void_ptr/void_ptr.hpp"

#include <uv.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace amj::owned {
namespace void_ptr = amj::void_ptr;

// Owns an individually new-allocated libuv handle T, initially uninitialized.
// Not for loops, requests, embedded handles, or handles already being closed.
// For initialized handles, data must be null or an owned VoidPtr wrapper.
// Its payload may be borrowed. Uninitialized handles are deleted directly;
// their data is not inspected or freed.
//
// Call setInitialized() immediately after successful libuv initialization,
// before anything can throw. When adopting an already initialized handle,
// likewise call setInitialized() immediately after construction or reset.
// Construction and reset always set initialized to false; release clears it.
// Moves transfer both pointer and state; handle storage is never relocated.
//
// For initialized handles, reset/destruction schedules uv_close. The borrowed
// loop must remain alive and run until the close callback frees the allocation.
// Borrowed callback payloads must remain valid while callbacks can still run.
// All operations require the loop's owner thread. Do not close the handle
// elsewhere while this wrapper owns it. reset before destroying dependencies.
template <typename T>
class UvHandle {
    static_assert(
        std::is_same_v<T, uv_poll_t> || std::is_same_v<T, uv_timer_t> || std::is_same_v<T, uv_async_t>,
        "UvHandle only supports uv_poll_t, uv_timer_t, and uv_async_t"
    );

  public:
    UvHandle() = default;

    explicit UvHandle(std::unique_ptr<T> uvHandle) noexcept {
        this->ownedUvHandle = uvHandle.release();
    }

    ~UvHandle() {
        if (ownedUvHandle == nullptr) {
            return;
        }
        if (!initialized) {
            delete ownedUvHandle;
            return;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(ownedUvHandle), [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
            delete reinterpret_cast<T*>(uvHandle);
        });
    }

    UvHandle(const UvHandle&) = delete;
    UvHandle& operator=(const UvHandle&) = delete;

    UvHandle(UvHandle&& uvHandle) noexcept {
        this->ownedUvHandle = std::exchange(uvHandle.ownedUvHandle, nullptr);
        this->initialized = std::exchange(uvHandle.initialized, false);
    }

    UvHandle& operator=(UvHandle&& uvHandle) noexcept {
        if (this != &uvHandle) {
            if (ownedUvHandle != nullptr) {
                if (initialized) {
                    uv_close(reinterpret_cast<uv_handle_t*>(ownedUvHandle), [](uv_handle_t* uvHandle) noexcept {
                        delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
                        delete reinterpret_cast<T*>(uvHandle);
                    });
                } else {
                    delete ownedUvHandle;
                }
            }
            this->ownedUvHandle = std::exchange(uvHandle.ownedUvHandle, nullptr);
            this->initialized = std::exchange(uvHandle.initialized, false);
        }
        return *this;
    }

    T* get() const noexcept {
        return ownedUvHandle;
    }

    // Only call for a nonnull, successfully initialized handle.
    void setInitialized() noexcept {
        initialized = true;
    }

    // The replacement must not be the currently owned pointer, unless null.
    // Adopts the replacement as uninitialized, even if it was initialized before.
    void reset(std::unique_ptr<T> uvHandle) noexcept {
        T* previousUvHandle = std::exchange(ownedUvHandle, uvHandle.release());
        const bool previousInitialized = std::exchange(initialized, false);
        if (previousUvHandle == nullptr) {
            return;
        }
        if (!previousInitialized) {
            delete previousUvHandle;
            return;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(previousUvHandle), [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<void_ptr::VoidPtr*>(uvHandle->data);
            delete reinterpret_cast<T*>(uvHandle);
        });
    }

    // Transfers responsibility for deletion or uv_close, according to state.
    // The recipient must already know whether initialization succeeded.
    T* release() noexcept {
        initialized = false;
        return std::exchange(ownedUvHandle, nullptr);
    }

  private:
    T* ownedUvHandle = nullptr;
    bool initialized = false;
};

} // namespace amj::owned
