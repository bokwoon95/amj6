#pragma once

#include "void_ptr/void_ptr.hpp"

#include <uv.h>

#include <utility>

namespace amj::owned {
namespace void_ptr = amj::void_ptr;

// Owns an individually new-allocated, successfully initialized libuv handle T.
// Not for loops, requests, embedded handles, or handles already being closed.
// data must be null or an owned VoidPtr wrapper. Its payload may be borrowed.
//
// Adopt immediately after successful initialization, before anything can throw.
// Before initialization succeeds, use an ordinary unique_ptr<T> instead.
// Moves transfer the pointer; initialized handle storage is never relocated.
//
// reset/destruction schedules uv_close, not immediate deletion. The borrowed
// loop must remain alive and run until the close callback frees the allocation.
// Borrowed callback payloads must remain valid while callbacks can still run.
// All operations require the loop's owner thread. Do not close the handle
// elsewhere while this wrapper owns it. reset before destroying dependencies.
template <typename T>
class UvHandle {
  public:
    UvHandle() = default;

    explicit UvHandle(T* initializedUvHandle) noexcept {
        this->ownedUvHandle = initializedUvHandle;
    }

    ~UvHandle() {
        reset();
    }

    UvHandle(const UvHandle&) = delete;
    UvHandle& operator=(const UvHandle&) = delete;

    UvHandle(UvHandle&& uvHandle) noexcept {
        this->ownedUvHandle = uvHandle.release();
    }

    UvHandle& operator=(UvHandle&& uvHandle) noexcept {
        if (this != &uvHandle) {
            reset();
            this->ownedUvHandle = uvHandle.release();
        }
        return *this;
    }

    T* get() const noexcept {
        return ownedUvHandle;
    }

    void reset() noexcept {
        T* uvHandle = std::exchange(ownedUvHandle, nullptr);
        if (uvHandle == nullptr) {
            return;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(uvHandle), [](uv_handle_t* closingUvHandle) noexcept {
            delete static_cast<void_ptr::VoidPtr*>(closingUvHandle->data);
            delete reinterpret_cast<T*>(closingUvHandle);
        });
    }

    // The recipient becomes responsible for uv_close and eventual deletion.
    T* release() noexcept {
        return std::exchange(ownedUvHandle, nullptr);
    }

  private:
    T* ownedUvHandle = nullptr;
};

} // namespace amj::owned
