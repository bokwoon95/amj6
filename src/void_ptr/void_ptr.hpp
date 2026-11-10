#pragma once

#include <any>
#include <exception>
#include <memory>
#include <type_traits>
#include <typeinfo>

namespace amj::void_ptr {

// Type-checked storage for C userdata slots. Delete the wrapper during cleanup;
// it deletes owned payloads and leaves borrowed payloads alone.
//
// Construct with unique_ptr<T> to own a payload; get<T>() returns T&.
// Construct with T* to borrow a payload; get<T*>() returns T*.
//
// Cast userdata back with static_cast<VoidPtr*>(data), assuming it points to a
// live VoidPtr. get<T>() checks both payload type and ownership mode, terminating
// on mismatch. Owned payloads must be non-null; borrowed pointers may be null
// and must otherwise remain alive while in use. Construction may throw but
// never leaks a supplied owned payload.
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
        this->any = pointer;
        this->typeInfo = &typeid(T*);
    }

    template <typename T>
    explicit VoidPtr(std::unique_ptr<T> pointer) {
        static_assert(!std::is_pointer_v<T> && !std::is_array_v<T>);
        if (pointer == nullptr) {
            std::terminate();
        }
        this->any = pointer.get();
        this->typeInfo = &typeid(T);
        this->deleteFunction = [](std::any& any) noexcept {
            T* const* pointer = std::any_cast<T*>(&any);
            if (pointer == nullptr) {
                std::terminate();
            }
            delete *pointer;
        };
        static_cast<void>(pointer.release());
    }

    template <typename T>
    decltype(auto) get() const noexcept {
        if constexpr (std::is_pointer_v<T>) {
            if (this->deleteFunction != nullptr) {
                std::terminate(); // Pointer template arguments require borrowed storage.
            }
            const T* pointer = std::any_cast<T>(&this->any);
            if (pointer == nullptr) {
                std::terminate();
            }
            return T(*pointer);
        } else {
            if (this->deleteFunction == nullptr) {
                std::terminate(); // Non-pointer template arguments require owned storage.
            }
            T* const* pointer = std::any_cast<T*>(&this->any);
            if (pointer == nullptr || *pointer == nullptr) {
                std::terminate();
            }
            return **pointer;
        }
    }

    // Reports typeid(T) for owned objects and typeid(T*) for borrowed pointers.
    // name() is not necessarily human-readable.
    const std::type_info& type() const noexcept {
        return *this->typeInfo;
    }

private:
    std::any any = {}; // Contains a T*: owned when deleteFunction is non-null, otherwise borrowed.
    const std::type_info* typeInfo = nullptr; // Borrowed; type_info has static lifetime.
    void (*deleteFunction)(std::any&) noexcept = nullptr;
};

} // namespace amj::void_ptr
