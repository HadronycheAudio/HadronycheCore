#pragma once

#include "hadronyche/testhost/Host.h"

#include <cstddef>
#include <cstdio>
#include <cwchar>

namespace Hadronyche {
namespace TestHost {

struct Test
{
    const wchar_t* name;
    void (*function) ();
};

struct TestProgram
{
    const char* name;
    const Test* tests;
    size_t testCount;

    // Only run when asked for by name, because it reports a measurement instead of checking anything.
    void (*measureCpu) ();

    // Around loading the plug-in, for anything that has to be in place before it loads.
    void (*beforeLoading) () { nullptr };
    void (*afterFinishing) () { nullptr };
};

// The main of a test program:
//
//   <program> <path to the plug-in's DLL> [test name]
//   <program> --list
inline int runTests (int argc, wchar_t** argv, const TestProgram& program)
{
    if (argc >= 2 && std::wcscmp (argv[1], L"--list") == 0)
    {
        for (size_t i = 0; i < program.testCount; ++i)
            std::wprintf (L"%ls\n", program.tests[i].name);
        std::wprintf (L"cpu\n");
        return 0;
    }

    if (argc < 2)
    {
        std::printf ("Usage: %s <path to the plug-in's DLL> [test name]\n", program.name);
        return 2;
    }

    if (program.beforeLoading)
        program.beforeLoading ();

    if (!loadPlugin (argv[1]))
    {
        std::wprintf (L"Could not load the plug-in: %ls\n", argv[1]);
        if (program.afterFinishing)
            program.afterFinishing ();
        return 2;
    }

    const wchar_t* filter = argc >= 3 ? argv[2] : nullptr;
    bool foundTest = false;

    if (filter && std::wcscmp (filter, L"cpu") == 0)
    {
        foundTest = true;
        program.measureCpu ();
    }

    for (size_t i = 0; i < program.testCount; ++i)
    {
        const Test& test = program.tests[i];
        if (filter && std::wcscmp (filter, test.name) != 0)
            continue;

        foundTest = true;
        std::wprintf (L"[%ls]\n", test.name);
        std::fflush (stdout);
        test.function ();
    }

    unloadPlugin ();
    if (program.afterFinishing)
        program.afterFinishing ();

    if (!foundTest)
    {
        std::wprintf (L"No test named %ls. Use --list to see them.\n", filter);
        return 2;
    }

    std::printf ("\n%d failure(s)\n", getFailureCount ());
    return getFailureCount () == 0 ? 0 : 1;
}

} // namespace TestHost
} // namespace Hadronyche
