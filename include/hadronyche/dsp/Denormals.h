#pragma once

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
#endif
};

} // namespace Hadronyche
