#pragma once

#include <concepts>
#include <limits>

namespace amj::intlimit {

// Check mathematical results against T's bounds without performing unsafe
// arithmetic. Both operands must have the same integral type, excluding bool.
// Overflow means above max(); underflow means below lowest(), not loss of precision.

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool multiplyOverflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return b != 0 && a > std::numeric_limits<T>::max() / b;
    } else {
        if (a > 0 && b > 0) {
            return a > std::numeric_limits<T>::max() / b;
        }
        if (a < 0 && b < 0) {
            return a < std::numeric_limits<T>::max() / b;
        }
        return false;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool multiplyUnderflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return false;
    } else {
        if (a > 0 && b < 0) {
            return b < std::numeric_limits<T>::lowest() / a;
        }
        if (a < 0 && b > 0) {
            return a < std::numeric_limits<T>::lowest() / b;
        }
        return false;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool addOverflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return a > std::numeric_limits<T>::max() - b;
    } else {
        return b > 0 && a > std::numeric_limits<T>::max() - b;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool addUnderflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return false;
    } else {
        return b < 0 && a < std::numeric_limits<T>::lowest() - b;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool subtractOverflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return false;
    } else {
        return b < 0 && a > std::numeric_limits<T>::max() + b;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool subtractUnderflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return a < b;
    } else {
        return b > 0 && a < std::numeric_limits<T>::lowest() + b;
    }
}

// Division by zero is a separate error: callers must check it independently.
// This returns false for a zero divisor. Integer division cannot underflow.
template <std::integral T>
    requires(!std::same_as<T, bool>)
constexpr bool divisionOverflows(T a, T b) noexcept {
    if constexpr (std::unsigned_integral<T>) {
        return false;
    } else {
        return a == std::numeric_limits<T>::lowest() && b == -1;
    }
}

} // namespace amj::intlimit
