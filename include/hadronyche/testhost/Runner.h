#pragma once

#include "hadronyche/testhost/Host.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

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

// The main of a test program, through HADRONYCHE_TEST_MAIN:
//
//   <program> <path to the plug-in's binary> [test name]
//   <program> --list
//
// Everything prints with printf, because on macOS a stream refuses narrow output after wide output.
inline int runTests (int argc, wchar_t** argv, const TestProgram& program)
{
    if (argc >= 2 && std::wcscmp (argv[1], L"--list") == 0)
    {
        for (size_t i = 0; i < program.testCount; ++i)
            std::printf ("%ls\n", program.tests[i].name);
        std::printf ("cpu\n");
        return 0;
    }

    if (argc < 2)
    {
        std::printf ("Usage: %s <path to the plug-in's binary> [test name]\n", program.name);
        return 2;
    }

    if (program.beforeLoading)
        program.beforeLoading ();

    if (!loadPlugin (argv[1]))
    {
        std::printf ("Could not load the plug-in: %ls\n", argv[1]);
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
        std::printf ("[%ls]\n", test.name);
        std::fflush (stdout);
        test.function ();
    }

    unloadPlugin ();
    if (program.afterFinishing)
        program.afterFinishing ();

    if (!foundTest)
    {
        std::printf ("No test named %ls. Use --list to see them.\n", filter);
        return 2;
    }

    std::printf ("\n%d failure(s)\n", getFailureCount ());
    return getFailureCount () == 0 ? 0 : 1;
}

inline int runTests (int argc, char** argv, const TestProgram& program)
{
    std::vector<std::wstring> wide;
    std::vector<wchar_t*> arguments;
    for (int i = 0; i < argc; ++i)
    {
        std::wstring argument (std::strlen (argv[i]) + 1, L'\0');
        argument.resize (std::mbstowcs (argument.data (), argv[i], argument.size ()));
        wide.push_back (argument);
    }
    for (auto& argument : wide)
        arguments.push_back (argument.data ());
    return runTests (argc, arguments.data (), program);
}

} // namespace TestHost
} // namespace Hadronyche

// wmain on Windows, where arguments can be any path, and main elsewhere.
#if defined(_WIN32)
#define HADRONYCHE_TEST_MAIN int wmain (int argc, wchar_t** argv)
#else
#define HADRONYCHE_TEST_MAIN int main (int argc, char** argv)
#endif
