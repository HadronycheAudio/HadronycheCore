#pragma once

#include <cmath>
#include <cstdio>

// Measurements print as they go, so that a failure comes with its numbers.
inline int gFailures = 0;

inline void check (const char* what, bool ok)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++gFailures;
}

inline void checkNear (const char* what, double value, double expected, double tolerance = 1e-12)
{
    const bool ok = std::abs (value - expected) <= tolerance;
    std::printf ("  %s %s: %.15g, expected %.15g\n", ok ? "ok  " : "FAIL", what, value, expected);
    if (!ok)
        ++gFailures;
}
