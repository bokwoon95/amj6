#pragma once

#include <curl/curl.h>

#include <curl/easy.h>
#include <curl/multi.h>
#include <exception>
#include <utility>

namespace amj::owned {

// Owns an easy handle, including its registration with a borrowed multi handle.
// Adopt only unregistered handles. Use these registration methods exclusively;
// direct curl_multi_add_handle/remove_handle calls would invalidate tracking.
// The multi handle must outlive registration. Callback data, request headers,
// and borrowed request-body storage must remain valid through reset/destruction.
// Operations and destruction require the handle's owner thread.
class CurlEasyHandle {
  public:
    CurlEasyHandle() = default;

    explicit CurlEasyHandle(CURL* curlEasyHandle) noexcept {
        this->ownedCurlEasyHandle = curlEasyHandle;
    }

    ~CurlEasyHandle() {
        reset();
    }

    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;

    CurlEasyHandle(CurlEasyHandle&& curlEasyHandle) noexcept {
        this->ownedCurlEasyHandle = std::exchange(curlEasyHandle.ownedCurlEasyHandle, nullptr);
        this->curlMultiHandle = std::exchange(curlEasyHandle.curlMultiHandle, nullptr);
    }

    CurlEasyHandle& operator=(CurlEasyHandle&& curlEasyHandle) noexcept {
        if (this != &curlEasyHandle) {
            reset();
            this->ownedCurlEasyHandle = std::exchange(curlEasyHandle.ownedCurlEasyHandle, nullptr);
            this->curlMultiHandle = std::exchange(curlEasyHandle.curlMultiHandle, nullptr);
        }
        return *this;
    }

    CURL* get() const noexcept {
        return ownedCurlEasyHandle;
    }

    CURLMcode addToMulti(CURLM* curlMultiHandle) noexcept {
        if (this->curlMultiHandle != nullptr) {
            return CURLM_ADDED_ALREADY;
        }
        const CURLMcode curlMCode = curl_multi_add_handle(curlMultiHandle, ownedCurlEasyHandle);
        if (curlMCode == CURLM_OK) {
            this->curlMultiHandle = curlMultiHandle;
        }
        return curlMCode;
    }

    CURLMcode removeFromMulti() noexcept {
        if (curlMultiHandle == nullptr) {
            return CURLM_OK;
        }
        const CURLMcode curlMCode = curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle);
        if (curlMCode == CURLM_OK) {
            curlMultiHandle = nullptr;
        }
        return curlMCode;
    }

    void reset() noexcept {
        if (removeFromMulti() != CURLM_OK) {
            // Never destroy callback storage while still registered with curl.
            std::terminate();
        }
        if (ownedCurlEasyHandle != nullptr) {
            curl_easy_cleanup(ownedCurlEasyHandle);
            ownedCurlEasyHandle = nullptr;
        }
    }

  private:
    CURL* ownedCurlEasyHandle = nullptr;
    CURLM* curlMultiHandle = nullptr; // Borrowed while registered.
};

// Owns a multi handle, not the easy handles registered with it.
// Remove all easy handles before reset/destruction. Cleanup can invoke socket
// callbacks: keep their data and dependencies alive until cleanup returns.
// Operations and destruction require the handle's owner thread.
// Neither curl wrapper owns process-wide curl_global_init/cleanup state.
class CurlMultiHandle {
  public:
    CurlMultiHandle() = default;

    explicit CurlMultiHandle(CURLM* curlMultiHandle) noexcept {
        this->ownedCurlMultiHandle = curlMultiHandle;
    }

    ~CurlMultiHandle() {
        reset();
    }

    CurlMultiHandle(const CurlMultiHandle&) = delete;
    CurlMultiHandle& operator=(const CurlMultiHandle&) = delete;

    CurlMultiHandle(CurlMultiHandle&& curlMultiHandle) noexcept {
        this->ownedCurlMultiHandle = curlMultiHandle.release();
    }

    CurlMultiHandle& operator=(CurlMultiHandle&& curlMultiHandle) noexcept {
        if (this != &curlMultiHandle) {
            reset();
            this->ownedCurlMultiHandle = curlMultiHandle.release();
        }
        return *this;
    }

    CURLM* get() const noexcept {
        return ownedCurlMultiHandle;
    }

    void reset() noexcept {
        if (ownedCurlMultiHandle == nullptr) {
            return;
        }
        if (curl_multi_cleanup(ownedCurlMultiHandle) != CURLM_OK) {
            std::terminate();
        }
        ownedCurlMultiHandle = nullptr;
    }

    // Registered easy handles still borrow this multi; preserve its lifetime.
    CURLM* release() noexcept {
        return std::exchange(ownedCurlMultiHandle, nullptr);
    }

  private:
    CURLM* ownedCurlMultiHandle = nullptr;
};

} // namespace amj::owned
