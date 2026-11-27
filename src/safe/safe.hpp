#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace amj::safe {

// Text is borrowed and must remain valid until printing finishes.
struct PrintArg {
    PrintArg(std::string_view text) noexcept : value(text) {}

    PrintArg(const char* text) noexcept
        : value(text == nullptr ? std::string_view() : std::string_view(text)) {}

    PrintArg(const std::string& text) noexcept : value(std::string_view(text)) {}

    PrintArg(bool boolean) noexcept : value(boolean) {}

    template <std::signed_integral T>
        requires(sizeof(T) <= sizeof(std::int64_t))
    PrintArg(T number) noexcept : value(static_cast<std::int64_t>(number)) {}

    template <std::unsigned_integral T>
        requires(sizeof(T) <= sizeof(std::uint64_t))
    PrintArg(T number) noexcept : value(static_cast<std::uint64_t>(number)) {}

    template <std::floating_point T>
    PrintArg(T number) noexcept : value(static_cast<long double>(number)) {}

    PrintArg(std::source_location sourceLocation) noexcept : value(sourceLocation) {}

    void print(std::ostream& out) const;

    const std::variant<std::string_view, bool, std::int64_t, std::uint64_t, long double,
        std::source_location>
        value = {};
};

// Inserts a space between adjacent non-string arguments. No newline is appended.
// Output failures are contained; argument evaluation before entry remains outside the catch.
void print(std::ostream& out, std::span<const PrintArg> printArgs) noexcept;

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
    const std::source_location sourceLocation = {};

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

private:
    Borrower* previous = nullptr;
    Borrower* next = nullptr;
};

void printStackTrace(std::ostream& out, size_t skip, size_t maxDepth) noexcept;

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
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Access through an empty or moved borrow\n"}));
            printStackTrace(std::cerr, 1, 30);
            std::terminate();
        }
        return resource;
    }

    [[nodiscard]] Borrowed<T>
    borrow(std::source_location sourceLocation = std::source_location::current()) const noexcept {
        if (borrower.empty()) {
            print(std::cerr, std::to_array<PrintArg>({sourceLocation,
                                 " Borrowing through an empty or moved borrow\n"}));
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
