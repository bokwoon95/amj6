#pragma once

#include <source_location>
#include <utility>

namespace amj::safe {

// The owner embeds a sentinel named head; every Borrowed<T> embeds another node.
// Empty/unlinked nodes point to themselves. All borrowing, access, movement,
// and closure must be serialized. Raw C resources must not escape their borrow.
struct Borrower {
    Borrower() noexcept {
        previous = this;
        next = this;
    };

    ~Borrower() {
        previous->next = next;
        next->previous = previous;
    };
    Borrower(const Borrower&) = delete;
    Borrower& operator=(const Borrower&) = delete;
    Borrower(Borrower&& other) noexcept {
        sourceLocation = other.sourceLocation;
        if (other.next == &other) {
            previous = this;
            next = this;
        } else {
            previous = other.previous;
            next = other.next;
            previous->next = this;
            next->previous = this;
            other.previous = &other;
            other.next = &other;
        }
    };
    Borrower& operator=(Borrower&&) = delete;

    std::source_location sourceLocation = {};
    Borrower* previous = nullptr;
    Borrower* next = nullptr;
};

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation) noexcept;

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation,
    const Borrower& head) noexcept;

template <typename T> class Borrowed {
public:
    Borrowed() = default;

    Borrowed(std::source_location sourceLocation, Borrower& previous,
        decltype(std::declval<const T&>().get()) resource) noexcept {
        this->resource = resource;
        borrower.sourceLocation = sourceLocation;
        borrower.previous = &previous;
        borrower.next = previous.next;
        borrower.previous->next = &borrower;
        borrower.next->previous = &borrower;
    }

    ~Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& other) noexcept : borrower(std::move(other.borrower)) {
        resource = other.resource;
    }
    Borrowed& operator=(Borrowed&&) = delete;

    [[nodiscard]] decltype(std::declval<const T&>().get())
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.next == &borrower) {
            fail("access through an empty or moved borrow", sourceLocation);
        }
        return resource;
    }

    [[nodiscard]] Borrowed<T>
    borrow(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.next == &borrower) {
            fail("borrowing through an empty or moved borrow", sourceLocation);
        }
        return Borrowed<T>(sourceLocation, *borrower.next, resource);
    }

private:
    Borrower borrower = {};
    decltype(std::declval<const T&>().get()) resource = {};
};

} // namespace amj::safe
