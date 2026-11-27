#pragma once

#include <exception>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
#include <utility>

namespace amj::safe {

// The owner embeds a sentinel named head; every Borrowed<T> embeds another node.
// Empty/unlinked nodes point to themselves. All borrowing, access, movement,
// and closure must be serialized. Raw C resources must not escape their borrow.
class Borrower {
    struct Iterator {
        const Borrower* current = nullptr;
        const Borrower& operator*() const noexcept {
            return *current;
        }
        Iterator& operator++() noexcept {
            current = current->next;
            return *this;
        }
        bool operator==(const Iterator& other) const noexcept {
            return current == other.current;
        }
    };

public:
    Borrower() noexcept : previous(this), next(this) {}

    Borrower(std::source_location sourceLocation, Borrower& previous) noexcept
        : sourceLocation(sourceLocation), previous(&previous), next(previous.next) {
        this->previous->next = this;
        next->previous = this;
    }

    ~Borrower() {
        previous->next = next;
        next->previous = previous;
    }
    Borrower(const Borrower&) = delete;
    Borrower& operator=(const Borrower&) = delete;
    Borrower(Borrower&& other) noexcept : sourceLocation(other.sourceLocation) {
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
    }
    Borrower& operator=(Borrower&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return next == this;
    }

    [[nodiscard]] Iterator begin() const noexcept {
        return Iterator{
            .current = next,
        };
    }

    [[nodiscard]] Iterator end() const noexcept {
        return Iterator{
            .current = this,
        };
    }

    [[nodiscard]] std::string toString() const {
        std::ostringstream buf;
        buf << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
            << sourceLocation.function_name() << ")";
        return buf.str();
    }

private:
    std::source_location sourceLocation = {};
    Borrower* previous = nullptr;
    Borrower* next = nullptr;
};

// skip counts frames to omit, including this helper; maxDepth limits capture.
// Trace and output failures are contained; this helper never terminates callers.
void printStackTrace(std::ostream& out, int skip, int maxDepth) noexcept;

template <typename T> class Borrowed {
public:
    Borrowed() = default;

    Borrowed(std::source_location sourceLocation, Borrower& previous,
        decltype(std::declval<const T&>().get()) resource) noexcept
        : borrower(sourceLocation, previous), resource(resource) {}

    ~Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& other) noexcept
        : borrower(std::move(other.borrower)), resource(other.resource) {}
    Borrowed& operator=(Borrowed&&) = delete;

    [[nodiscard]] decltype(std::declval<const T&>().get())
    get(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.empty()) {
            try {
                std::cerr << "Access through an empty or moved borrow at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return resource;
    }

    [[nodiscard]] Borrowed<T>
    borrow(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.empty()) {
            try {
                std::cerr << "Borrowing through an empty or moved borrow at "
                          << sourceLocation.file_name() << ':' << sourceLocation.line() << " ("
                          << sourceLocation.function_name() << ')' << '\n'
                          << std::flush;
            } catch (...) {
                printStackTrace(std::cerr, 1, 30);
                std::terminate();
            }
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return Borrowed<T>(sourceLocation, borrower, resource);
    }

private:
    // Reborrowing a const handle changes only its list bookkeeping, not resource.
    mutable Borrower borrower = {};
    decltype(std::declval<const T&>().get()) resource = {};
};

} // namespace amj::safe
