#include "safe.hpp"

#include <cpptrace/basic.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <ostream>
#include <source_location>
#include <span>
#include <string_view>
#include <variant>

namespace amj::safe {

void PrintArg::print(std::ostream& out) const {
    static_assert(std::variant_size_v<decltype(value)> == 6);
    switch (value.index()) {
        case 0: {
            const std::string_view* text = std::get_if<0>(&value);
            if (text != nullptr) {
                out << *text;
                return;
            }
            break;
        }
        case 1: {
            const bool* boolean = std::get_if<1>(&value);
            if (boolean != nullptr) {
                out << *boolean;
                return;
            }
            break;
        }
        case 2: {
            const std::int64_t* number = std::get_if<2>(&value);
            if (number != nullptr) {
                out << *number;
                return;
            }
            break;
        }
        case 3: {
            const std::uint64_t* number = std::get_if<3>(&value);
            if (number != nullptr) {
                out << *number;
                return;
            }
            break;
        }
        case 4: {
            const long double* number = std::get_if<4>(&value);
            if (number != nullptr) {
                out << *number;
                return;
            }
            break;
        }
        case 5: {
            const std::source_location* sourceLocation = std::get_if<5>(&value);
            if (sourceLocation != nullptr) {
                out << sourceLocation->file_name() << ':' << sourceLocation->line() << ':'
                    << sourceLocation->column() << ':' << sourceLocation->function_name();
                return;
            }
            break;
        }
        default: {
            break;
        }
    }
    out << "<invalid PrintArg>";
}

void fprintln(std::ostream& out, std::span<const PrintArg> printArgs) noexcept {
    try {
        bool written = false;
        for (const PrintArg& printArg : printArgs) {
            if (!written) {
                written = true;
            } else {
                out << ' ';
            }
            printArg.print(out);
        }
        out << '\n' << std::flush;
    } catch (const std::exception& exception) {
        std::fputs(exception.what(), stderr);
        std::fputc('\n', stderr);
    } catch (...) {
        std::fputs("exception thrown\n", stderr);
    }
}

void printStackTrace(std::ostream& out, int skip, int maxDepth) noexcept {
    try {
        if (skip < 0 || maxDepth < 0) {
            out << "Invalid stack trace limits\n" << std::flush;
            return;
        }
        cpptrace::generate_trace(static_cast<std::size_t>(skip), static_cast<std::size_t>(maxDepth))
            .print(out);
    } catch (...) {
        try {
            out << "Unable to print stack trace\n" << std::flush;
        } catch (...) {
            // Diagnostic output is best effort. The caller still terminates.
            return;
        }
    }
}

} // namespace amj::safe
