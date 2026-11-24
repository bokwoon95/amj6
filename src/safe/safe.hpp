#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace amj::safe {

// Lifetime checks are not synchronization. Access and closure must be serialized,
// and a raw value returned by get() must not outlive its checked access scope.
enum class ResourceState : std::uint8_t { Live, Closing, Closed };

struct Lifetime {
    ResourceState state = ResourceState::Live;
    std::string creationTrace = "";
    std::string closureTrace = "";
    std::error_code closeError = {};
};

std::string captureTrace() noexcept;
[[noreturn]] void fail(const char* reason, const Lifetime* lifetime) noexcept;

template <typename T> class Owned;

template <typename T> class Borrowed {
public:
    explicit Borrowed(const Owned<T>& owner) : item(owner.get()), lifetime(owner.lifetimeToken()) {}

    [[nodiscard]] T get() const {
        if (lifetime == nullptr || lifetime->state != ResourceState::Live) {
            fail("access through an expired borrower", lifetime.get());
        }
        return item;
    }

private:
    T item = {};
    std::shared_ptr<const Lifetime> lifetime = nullptr;
};

template <typename T> class Owned {
    static_assert(std::is_nothrow_copy_constructible_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    // Adoption succeeds only when construction completes. On failure this
    // constructor invokes closeItem itself, so callers must not clean up twice.
    // The callable is copied before acquisition completes; no raw std::function
    // conversion is required at the callsite. Argument evaluation remains the
    // caller's responsibility (in particular, throwing capture initializers).
    template <typename Close> Owned(T item, Close&& closeItem) {
        if constexpr (std::is_pointer_v<T>) {
            if (item == nullptr) {
                fail("cannot own a null pointer", nullptr);
            }
        }
        try {
            closeFunction = closeItem;
            if (!closeFunction) {
                fail("missing cleanup function", nullptr);
            }
            lifetime = std::make_shared<Lifetime>();
            lifetime->creationTrace = captureTrace();
            this->item = item;
        } catch (...) {
            try {
                const std::error_code error = std::invoke(closeItem, item);
                if (error) {
                    fail("cleanup failed during adoption", nullptr);
                }
            } catch (...) {
                fail("cleanup threw during adoption", nullptr);
            }
            throw;
        }
    }

    ~Owned() noexcept {
        const bool needsClose = lifetime != nullptr && lifetime->state == ResourceState::Live;
        const std::error_code error = close();
        if (needsClose && error) {
            fail("unhandled destructor cleanup error", lifetime.get());
        }
    }

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    Owned(Owned&&) noexcept = default;
    Owned& operator=(Owned&&) = delete;

    [[nodiscard]] T get() const {
        if (lifetime == nullptr || lifetime->state != ResourceState::Live) {
            fail("access through a closed or moved owner", lifetime.get());
        }
        if (!item.has_value()) {
            fail("owner has no resource", lifetime.get());
        }
        return item.value();
    }

    [[nodiscard]] Borrowed<T> borrow() const {
        return Borrowed<T>(*this);
    }

    // Internal observation hook; exposes metadata, never ownership rights.
    [[nodiscard]] std::shared_ptr<const Lifetime> lifetimeToken() const noexcept {
        return lifetime;
    }

    // Normal return discharges ownership, even on error. Cleanup may transfer
    // physical deletion to an asynchronous library callback.
    [[nodiscard]] std::error_code close() noexcept {
        if (lifetime == nullptr) {
            return {};
        }
        if (lifetime->state == ResourceState::Closing) {
            fail("reentrant closure", lifetime.get());
        }
        if (lifetime->state == ResourceState::Closed) {
            return lifetime->closeError;
        }
        lifetime->state = ResourceState::Closing;
        lifetime->closureTrace = captureTrace();
        if (!item.has_value()) {
            fail("owner has no resource during closure", lifetime.get());
        }
        try {
            lifetime->closeError = closeFunction(item.value());
        } catch (...) {
            fail("cleanup callback threw", lifetime.get());
        }
        item.reset();
        lifetime->state = ResourceState::Closed;
        closeFunction = nullptr;
        return lifetime->closeError;
    }

private:
    std::optional<T> item = std::nullopt;
    std::function<std::error_code(T)> closeFunction = {};
    std::shared_ptr<Lifetime> lifetime = nullptr;
};

// Typed C userdata storage. Owning payloads are values; pointer payloads borrow.
// Type checking does not track raw borrowed pointee lifetime. Store Borrowed<T>
// as a value payload when lifetime checking is also required.
class VoidPtr {
public:
    VoidPtr() = default;
    VoidPtr(const VoidPtr&) = delete;
    VoidPtr& operator=(const VoidPtr&) = delete;
    VoidPtr(VoidPtr&&) = delete;
    VoidPtr& operator=(VoidPtr&&) = delete;

    ~VoidPtr() {
        if (deleteFunction != nullptr) {
            deleteFunction(any);
        }
    }

    template <typename T> explicit VoidPtr(T* pointer) {
        assign(pointer);
    }

    template <typename T>
        requires(!std::is_lvalue_reference_v<T> && !std::is_pointer_v<std::remove_cvref_t<T>>)
    explicit VoidPtr(T&& value) {
        assign(std::forward<T>(value));
    }

    template <typename T> void assign(T* pointer) {
        std::any replacement = pointer;
        if (deleteFunction != nullptr) {
            deleteFunction(any);
        }
        any.swap(replacement);
        deleteFunction = nullptr;
    }

    template <typename T>
        requires(!std::is_lvalue_reference_v<T> && !std::is_pointer_v<std::remove_cvref_t<T>>)
    void assign(T&& value) {
        static_assert(!std::is_array_v<T>);
        std::any replacement = static_cast<T*>(nullptr);
        std::unique_ptr<T> pointer = std::make_unique<T>(std::forward<T>(value));
        T** replacementPointer = std::any_cast<T*>(&replacement);
        if (replacementPointer == nullptr) {
            fail("invalid userdata storage", nullptr);
        }
        *replacementPointer = pointer.release();
        if (deleteFunction != nullptr) {
            deleteFunction(any);
        }
        any.swap(replacement);
        deleteFunction = [](std::any& any) noexcept {
            T* const* pointer = std::any_cast<T*>(&any);
            if (pointer == nullptr) {
                fail("invalid owned userdata type", nullptr);
            }
            delete *pointer;
        };
    }

    template <typename T> bool is() const noexcept {
        if constexpr (std::is_pointer_v<T>) {
            return deleteFunction == nullptr && std::any_cast<T>(&any) != nullptr;
        } else {
            return deleteFunction != nullptr && std::any_cast<T*>(&any) != nullptr;
        }
    }

    template <typename T> decltype(auto) cast() const noexcept {
        if (!is<T>()) {
            fail("userdata type or ownership mismatch", nullptr);
        }
        if constexpr (std::is_pointer_v<T>) {
            const T* pointer = std::any_cast<T>(&any);
            if (pointer == nullptr) {
                fail("invalid borrowed userdata", nullptr);
            }
            return T(*pointer);
        } else {
            T* const* pointer = std::any_cast<T*>(&any);
            if (pointer == nullptr || *pointer == nullptr) {
                fail("invalid owned userdata", nullptr);
            }
            return **pointer;
        }
    }

private:
    std::any any = {};
    void (*deleteFunction)(std::any&) noexcept = nullptr;
};

} // namespace amj::safe
