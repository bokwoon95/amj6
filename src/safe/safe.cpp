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
#include <system_error>
#include <variant>

namespace amj::safe {

void PrintArg::print(std::ostream& out) const {
    static_assert(std::variant_size_v<decltype(value)> == 7);
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
        case 6: {
            const std::error_code* errorCode = std::get_if<6>(&value);
            if (errorCode != nullptr) {
                out << errorCode->message();
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

void print(std::ostream& out, std::span<const PrintArg> printArgs) noexcept {
    try {
        bool wasString = true;
        for (const PrintArg& printArg : printArgs) {
            const bool isString = std::holds_alternative<std::string_view>(printArg.value);
            if (!wasString && !isString) {
                out << ' ';
            }
            printArg.print(out);
            wasString = isString;
        }
        out << std::flush;
    } catch (const std::exception& exception) {
        std::fputs(exception.what(), stderr);
        std::fputc('\n', stderr);
    } catch (...) {
        std::fputs("exception thrown\n", stderr);
    }
}

void printStackTrace(std::ostream& out, size_t skip, size_t maxDepth) noexcept {
    try {
        cpptrace::generate_trace(skip, maxDepth).print(out);
    } catch (const std::exception& exception) {
        std::fputs(exception.what(), stderr);
        std::fputc('\n', stderr);
    } catch (...) {
        std::fputs("exception thrown\n", stderr);
    }
}

} // namespace amj::safe
