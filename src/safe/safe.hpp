#pragma once

#include <source_location>
#include <utility>

namespace amj::safe {

// The owner embeds a sentinel named head; every Borrowed<T> embeds another node.
// Empty/unlinked nodes point to themselves. All borrowing, access, movement,
// and closure must be serialized. Raw C resources must not escape their borrow.
struct Borrower {
    Borrower() noexcept;

    ~Borrower();
    Borrower(const Borrower&) = delete;
    Borrower& operator=(const Borrower&) = delete;
    Borrower(Borrower&& other) noexcept;
    Borrower& operator=(Borrower&&) = delete;

    Borrower* previous = nullptr;
    Borrower* next = nullptr;
    std::source_location sourceLocation = {};
};

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation) noexcept;

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation,
    const Borrower& head) noexcept;

template <typename T> class Borrowed {
public:
    Borrowed() = default;

    [[nodiscard]] static Borrowed<T> makeBorrowed(decltype(std::declval<const T&>().get()) item,
        Borrower& neighbor, std::source_location sourceLocation) noexcept {
        Borrowed<T> borrowed;
        borrowed.item = item;
        borrowed.borrower.sourceLocation = sourceLocation;
        borrowed.borrower.previous = &neighbor;
        borrowed.borrower.next = neighbor.next;
        borrowed.borrower.previous->next = &borrowed.borrower;
        borrowed.borrower.next->previous = &borrowed.borrower;
        return borrowed;
    }

    ~Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& other) noexcept : item(other.item), borrower(std::move(other.borrower)) {}
    Borrowed& operator=(Borrowed&&) = delete;

    [[nodiscard]] decltype(std::declval<const T&>().get())
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.next == &borrower) {
            fail("access through an empty or moved borrow", sourceLocation);
        }
        return item;
    }

    [[nodiscard]] Borrowed<T>
    borrow(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        return makeBorrowed(this->get(sourceLocation), *this->borrower.next, sourceLocation);
    }

private:
    decltype(std::declval<const T&>().get()) item = {};
    Borrower borrower = {};
};

} // namespace amj::safe
