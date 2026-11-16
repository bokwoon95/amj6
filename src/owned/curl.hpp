#pragma once

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include <exception>
#include <utility>

namespace amj::owned {

// Owns an easy handle and removes any tracked registration before cleanup.
// Adopt only unregistered handles. After successful raw curl add/remove calls,
// immediately update curlMultiHandle before anything can throw. The borrowed
// multi handle must outlive registration. Callback data, request headers, and
// borrowed request-body storage must remain valid through reset/destruction.
// Operations and destruction require the handle's owner thread.
class CurlEasyHandle {
  public:
    CURLM* curlMultiHandle = nullptr; // Borrowed; nonnull only while successfully registered.

    CurlEasyHandle() = default;

    explicit CurlEasyHandle(CURL* curlEasyHandle) noexcept {
        this->ownedCurlEasyHandle = curlEasyHandle;
    }

    ~CurlEasyHandle() {
        if (curlMultiHandle != nullptr) {
            if (curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle) != CURLM_OK) {
                std::terminate(); // Never destroy callback storage still registered with curl.
            }
        }
        if (ownedCurlEasyHandle != nullptr) {
            curl_easy_cleanup(ownedCurlEasyHandle);
        }
    }

    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;

    CurlEasyHandle(CurlEasyHandle&& curlEasyHandle) noexcept {
        this->ownedCurlEasyHandle = std::exchange(curlEasyHandle.ownedCurlEasyHandle, nullptr);
        this->curlMultiHandle = std::exchange(curlEasyHandle.curlMultiHandle, nullptr);
    }

    CurlEasyHandle& operator=(CurlEasyHandle&& curlEasyHandle) noexcept {
        if (this != &curlEasyHandle) {
            if (curlMultiHandle != nullptr) {
                if (curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle) != CURLM_OK) {
                    std::terminate(); // Never destroy callback storage still registered with curl.
                }
            }
            if (ownedCurlEasyHandle != nullptr) {
                curl_easy_cleanup(ownedCurlEasyHandle);
            }
            this->ownedCurlEasyHandle = std::exchange(curlEasyHandle.ownedCurlEasyHandle, nullptr);
            this->curlMultiHandle = std::exchange(curlEasyHandle.curlMultiHandle, nullptr);
        }
        return *this;
    }

    CURL* get() const noexcept {
        return ownedCurlEasyHandle;
    }

    // Unregisters and cleans up the handle, clearing both pointers.
    void reset() noexcept {
        if (curlMultiHandle != nullptr) {
            if (curl_multi_remove_handle(curlMultiHandle, ownedCurlEasyHandle) != CURLM_OK) {
                std::terminate(); // Never destroy callback storage still registered with curl.
            }
            curlMultiHandle = nullptr;
        }
        if (ownedCurlEasyHandle != nullptr) {
            curl_easy_cleanup(ownedCurlEasyHandle);
        }
        ownedCurlEasyHandle = nullptr;
    }

    // Transfers cleanup and unregistration responsibility. The recipient must
    // already know the registered multi handle; this clears tracking only.
    CURL* release() noexcept {
        curlMultiHandle = nullptr;
        return std::exchange(ownedCurlEasyHandle, nullptr);
    }

  private:
    CURL* ownedCurlEasyHandle = nullptr;
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
        if (ownedCurlMultiHandle != nullptr) {
            if (curl_multi_cleanup(ownedCurlMultiHandle) != CURLM_OK) {
                std::terminate();
            }
        }
    }

    CurlMultiHandle(const CurlMultiHandle&) = delete;
    CurlMultiHandle& operator=(const CurlMultiHandle&) = delete;

    CurlMultiHandle(CurlMultiHandle&& curlMultiHandle) noexcept {
        this->ownedCurlMultiHandle = std::exchange(curlMultiHandle.ownedCurlMultiHandle, nullptr);
    }

    CurlMultiHandle& operator=(CurlMultiHandle&& curlMultiHandle) noexcept {
        if (this != &curlMultiHandle) {
            if (ownedCurlMultiHandle != nullptr) {
                if (curl_multi_cleanup(ownedCurlMultiHandle) != CURLM_OK) {
                    std::terminate();
                }
            }
            this->ownedCurlMultiHandle = std::exchange(curlMultiHandle.ownedCurlMultiHandle, nullptr);
        }
        return *this;
    }

    CURLM* get() const noexcept {
        return ownedCurlMultiHandle;
    }

    // Cleans up and clears the handle. Remove all easy handles first.
    void reset() noexcept {
        if (ownedCurlMultiHandle != nullptr) {
            if (curl_multi_cleanup(ownedCurlMultiHandle) != CURLM_OK) {
                std::terminate();
            }
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
