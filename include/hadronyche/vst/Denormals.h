#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace Hadronyche {

// Stops very small values in the filters from slowing the CPU down, for as long as it's in scope.
class FlushDenormals
{
public:
#if defined(_M_X64) || defined(__x86_64__)
    FlushDenormals () : _previous (_mm_getcsr ()) { _mm_setcsr (_previous | 0x8040); }
    ~FlushDenormals () { _mm_setcsr (_previous); }

private:
    unsigned int _previous;
#elif defined(__aarch64__)
    FlushDenormals ()
    {
        uint64_t control = 0;
        __asm__ volatile ("mrs %0, fpcr" : "=r" (control));
        _previous = control;
        control |= uint64_t (1) << 24; // FZ, which flushes denormals to zero.
        __asm__ volatile ("msr fpcr, %0" : : "r" (control));
    }

    ~FlushDenormals ()
    {
        __asm__ volatile ("msr fpcr, %0" : : "r" (_previous));
    }

private:
    uint64_t _previous;
#endif
};

} // namespace Hadronyche
