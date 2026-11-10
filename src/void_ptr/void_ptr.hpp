#pragma once

#include <any>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace amj::void_ptr {

// Type-checked storage for C userdata slots. Delete the wrapper during cleanup;
// it deletes owned payloads and leaves borrowed payloads alone.
//
// Construct or assign with unique_ptr<T> to own a payload; cast<T>() returns T&.
// Construct or assign with T* to borrow a payload; cast<T*>() returns T*.
// Assignment replaces the payload without reallocating the wrapper and destroys
// the previous payload if owned. Allocation failure leaves the old payload intact.
//
// is<T>() checks both payload type and ownership mode. cast<T>() terminates on
// mismatch. Owned payloads must be non-null; borrowed pointers may be null and
// must otherwise remain alive while in use. Construction and assignment may
// throw but never leak a supplied owned payload.
class VoidPtr {
public:
    VoidPtr(const VoidPtr&) = delete;
    VoidPtr& operator=(const VoidPtr&) = delete;
    VoidPtr(VoidPtr&&) = delete;
    VoidPtr& operator=(VoidPtr&&) = delete;

    ~VoidPtr() {
        if (this->deleteFunction != nullptr) {
            this->deleteFunction(this->any);
        }
    }

    template <typename T>
    explicit VoidPtr(T* pointer) {
        this->assign(pointer);
    }

    template <typename T>
    explicit VoidPtr(std::unique_ptr<T> pointer) {
        this->assign(std::move(pointer));
    }

    template <typename T>
    void assign(T* pointer) {
        std::any replacement = pointer;
        if (this->deleteFunction != nullptr) {
            this->deleteFunction(this->any);
        }
        this->any.swap(replacement);
        this->deleteFunction = nullptr;
    }

    template <typename T>
    void assign(std::unique_ptr<T> pointer) {
        static_assert(!std::is_pointer_v<T> && !std::is_array_v<T>);
        if (pointer == nullptr) {
            std::terminate();
        }
        std::any replacement = pointer.get();
        T** replacementPointer = std::any_cast<T*>(&replacement);
        if (replacementPointer == nullptr) {
            std::terminate();
        }
        *replacementPointer = pointer.release();
        if (this->deleteFunction != nullptr) {
            this->deleteFunction(this->any);
        }
        this->any.swap(replacement);
        this->deleteFunction = [](std::any& any) noexcept {
            T* const* pointer = std::any_cast<T*>(&any);
            if (pointer == nullptr) {
                std::terminate();
            }
            delete *pointer;
        };
    }

    template <typename T>
    bool is() const noexcept {
        if constexpr (std::is_pointer_v<T>) {
            return this->deleteFunction == nullptr && std::any_cast<T>(&this->any) != nullptr;
        } else {
            return this->deleteFunction != nullptr && std::any_cast<T*>(&this->any) != nullptr;
        }
    }

    template <typename T>
    decltype(auto) cast() const noexcept {
        if (!this->is<T>()) {
            std::terminate();
        }
        if constexpr (std::is_pointer_v<T>) {
            const T* pointer = std::any_cast<T>(&this->any);
            if (pointer == nullptr) {
                std::terminate();
            }
            return T(*pointer);
        } else {
            T* const* pointer = std::any_cast<T*>(&this->any);
            if (pointer == nullptr || *pointer == nullptr) {
                std::terminate();
            }
            return **pointer;
        }
    }

private:
    std::any any = {}; // Contains a T*: owned when deleteFunction is non-null, otherwise borrowed.
    void (*deleteFunction)(std::any&) noexcept = nullptr;
};

// Accept only actual void* or const void* userdata. For an already-typed wrapper,
// call is<T>() or cast<T>() directly. Non-null data must point to a live VoidPtr.
// A null wrapper does not match any type; a borrowed payload may itself be null.
template <typename T, typename Pointer>
    requires(std::is_same_v<Pointer, void*> || std::is_same_v<Pointer, const void*>)
bool is(Pointer data) noexcept {
    return data != nullptr && static_cast<const VoidPtr*>(data)->is<T>();
}

// A null wrapper is invalid; a borrowed payload inside it may still be null.
template <typename T, typename Pointer>
    requires(std::is_same_v<Pointer, void*> || std::is_same_v<Pointer, const void*>)
decltype(auto) cast(Pointer data) noexcept {
    if (data == nullptr) {
        std::terminate();
    }
    return static_cast<const VoidPtr*>(data)->cast<T>();
}

} // namespace amj::void_ptr
