#include "uv.hpp"

#include "safe.hpp"

#include <uv.h>

#include <expected>
#include <memory>
#include <string>
#include <system_error>

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

template <typename T, typename Initialize>
std::expected<Owned<T*>, std::error_code> createUvHandle(const Initialize& initialize) {
    const std::shared_ptr<bool> initialized = std::make_shared<bool>(false);
    Owned<T*> handle(new T(), [initialized](T* handle) noexcept -> std::error_code {
        if (!*initialized) {
            delete handle;
            return {};
        }
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* uvHandle) noexcept {
            delete static_cast<VoidPtr*>(uvHandle->data);
            delete reinterpret_cast<T*>(uvHandle);
        });
        return {};
    });
    const int status = initialize(handle.get());
    if (status != 0) {
        return std::unexpected(uvError(status));
    }
    *initialized = true;
    handle.get()->data = nullptr;
    return handle;
}
} // namespace

std::error_code uvError(int status) noexcept {
    return {status, uvCategory};
}

std::expected<Owned<uv_timer_t*>, std::error_code>
createUvTimer(const Borrowed<uv_loop_t*>& uvLoop) {
    return createUvHandle<uv_timer_t>([uvLoop](uv_timer_t* handle) {
        return uv_timer_init(uvLoop.get(), handle);
    });
}

std::expected<Owned<uv_async_t*>, std::error_code> createUvAsync(const Borrowed<uv_loop_t*>& uvLoop,
    uv_async_cb callback) {
    return createUvHandle<uv_async_t>([uvLoop, callback](uv_async_t* handle) {
        return uv_async_init(uvLoop.get(), handle, callback);
    });
}

// uv.h selects uv/unix.h or uv/win.h; include the public header, not platform internals.
std::expected<Owned<uv_poll_t*>, std::error_code> createUvPoll(const Borrowed<uv_loop_t*>& uvLoop,
    uv_os_sock_t socket) { // NOLINT(misc-include-cleaner)

    return createUvHandle<uv_poll_t>([uvLoop, socket](uv_poll_t* handle) {
        return uv_poll_init_socket(uvLoop.get(), handle, socket);
    });
}

} // namespace amj::safe
