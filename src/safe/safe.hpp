#pragma once

#include <source_location>
#include <utility>

namespace amj::safe {

// The owner embeds a sentinel named head; every Borrowed<T> embeds another node.
// Empty/unlinked nodes point to themselves. All borrowing, access, movement,
// and closure must be serialized. Raw C resources must not escape their borrow.
struct Borrower {
    Borrower() noexcept;
    Borrower(Borrower& neighbor,
        std::source_location location = std::source_location::current()) noexcept;

    ~Borrower();
    Borrower(const Borrower&) = delete;
    Borrower& operator=(const Borrower&) = delete;
    Borrower(Borrower&& other) noexcept;
    Borrower& operator=(Borrower&&) = delete;

    Borrower* previous = nullptr;
    Borrower* next = nullptr;
    std::source_location sourceLocation = {};
};

[[noreturn]] void fail(const char* reason, std::source_location location) noexcept;

[[noreturn]] void fail(const char* reason, std::source_location location,
    const Borrower& head) noexcept;

template <typename T> class Borrowed {
public:
    Borrowed() = default;

    Borrowed(decltype(std::declval<const T&>().get()) item, Borrower& neighbor,
        std::source_location location = std::source_location::current()) noexcept
        : item(item), borrower(neighbor, location) {}

    ~Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& other) noexcept : item(other.item), borrower(std::move(other.borrower)) {}
    Borrowed& operator=(Borrowed&&) = delete;

    [[nodiscard]] decltype(std::declval<const T&>().get())
    get(std::source_location location = std::source_location::current()) const noexcept {
        if (borrower.next == &borrower) {
            fail("access through an empty or moved borrow", location);
        }
        return item;
    }

    // Explicitly create an independently registered borrow beside this one's
    // neighbor. No owner or sentinel pointer is needed to insert into the list.
    [[nodiscard]] Borrowed<T>
    borrow(std::source_location location = std::source_location::current()) const noexcept {
        return Borrowed<T>(get(location), *borrower.next, location);
    }

private:
    decltype(std::declval<const T&>().get()) item = {};
    Borrower borrower = {};
};

} // namespace amj::safe
