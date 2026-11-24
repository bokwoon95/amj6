#include "safe.hpp"

#include <cpptrace/basic.hpp>

#include <cstdio>
#include <exception>
#include <string>

namespace amj::safe {

std::string captureTrace() noexcept {
    try {
        return cpptrace::generate_trace(1, 30).to_string();
    } catch (...) {
        return {};
    }
}

[[noreturn]] void fail(const char* reason, const Lifetime* lifetime) noexcept {
    std::fprintf(stderr, "Fatal lifetime violation: %s\n", reason);
    if (lifetime != nullptr) {
        std::fprintf(stderr, "Created:\n%s\nClosed:\n%s\n", lifetime->creationTrace.c_str(),
            lifetime->closureTrace.c_str());
        if (lifetime->closeError) {
            std::fprintf(stderr, "Cleanup error: %s:%d\n", lifetime->closeError.category().name(),
                lifetime->closeError.value());
        }
    }
    const std::string trace = captureTrace();
    std::fprintf(stderr, "Failure:\n%s\n", trace.c_str());
    std::fflush(stderr);
    std::terminate();
}

} // namespace amj::safe
