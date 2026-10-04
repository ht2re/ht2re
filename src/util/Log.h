#pragma once
#include <cstdio>
#include <cstdarg>

inline void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stdout, "[ht2re] ");
    std::vfprintf(stdout, fmt, ap);
    std::fprintf(stdout, "\n");
    va_end(ap);
}
#define LOG(...) ::logf(__VA_ARGS__)