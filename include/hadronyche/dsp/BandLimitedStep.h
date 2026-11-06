#pragma once

namespace Hadronyche {

// A band-limited step: the integral of a cubic B-spline, minus the ideal step it replaces. x is the distance
// from the step in samples, and the correction reaches zero 2 samples away.
//
// That reach only works up to a quarter of the sample rate, so an oscillator using this has to fade out
// by there.
inline double stepCorrection (double x)
{
    if (x < -1.0)
    {
        const double a = x + 2.0;
        return a * a * a * a / 24.0;
    }
    if (x < 0.0)
        return 0.5 + x * (2.0 / 3.0 - x * x * (1.0 / 3.0 + x / 8.0));
    if (x < 1.0)
        return -0.5 + x * (2.0 / 3.0 - x * x * (1.0 / 3.0 - x / 8.0));

    const double a = 2.0 - x;
    return -a * a * a * a / 24.0;
}

} // namespace Hadronyche
