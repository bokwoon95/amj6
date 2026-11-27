#include "safe.hpp"

#include <cpptrace/basic.hpp>

#include <cstddef>
#include <ostream>

namespace amj::safe {

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
