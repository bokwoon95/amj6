#include "safe.hpp"

#include <cpptrace/basic.hpp>

#include <cstdio>
#include <exception>
#include <source_location>

namespace amj::safe {

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation) noexcept {
    std::fprintf(stderr, "Fatal lifetime violation: %s\nAt %s:%u in %s\n", reason,
        sourceLocation.file_name(), sourceLocation.line(), sourceLocation.function_name());
    try {
        cpptrace::generate_trace(1, 30).print();
    } catch (...) {
        std::fprintf(stderr, "Unable to capture failure stack trace\n");
    }
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void fail(const char* reason, std::source_location sourceLocation,
    const Borrower& head) noexcept {
    std::fprintf(stderr, "Outstanding borrowers:\n");
    for (const Borrower* borrower = head.next; borrower != &head; borrower = borrower->next) {
        std::fprintf(stderr, "  %s:%u in %s\n", borrower->sourceLocation.file_name(),
            borrower->sourceLocation.line(), borrower->sourceLocation.function_name());
    }
    fail(reason, sourceLocation);
}

} // namespace amj::safe
