#pragma once

#include <cmath>
#include <cstdio>
#include <initializer_list>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Or windows.h's max macro breaks std::max in the headers under test.
#endif
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <windows.h>
#endif

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

// A Debug build on Windows stops in a dialog box when something asserts or aborts, which on CI is a hang until the
// job's time runs out. This sends those to stderr instead, and stops buffering the checks, so that the output says
// what happened and how far it got.
inline void failLoudly ()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int report : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode (report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile (report, _CRTDBG_FILE_STDERR);
    }
#endif
}
