#pragma once

#include <algorithm>
#include <cmath>

namespace Hadronyche {

// The kernel of a topology-preserving state-variable filter, which stays stable under fast modulation.
// Plug-ins build their own filters on it, with however many channels and stages they need.

// Plug-ins cast saved parameter values to this, so only ever add to the end.
enum class SvfMode
{
    LowPass,
    HighPass,
    BandPass,
    Notch
};

struct SvfCoefficients
{
    float k { 1.0f };
    float a1 { 0.0f };
    float a2 { 0.0f };
    float a3 { 0.0f };
};

struct SvfState
{
    float ic1 { 0.0f };
    float ic2 { 0.0f };
};

struct SvfTaps
{
    float v1;
    float v2;
};

// The prewarped frequency, held to what the filter can do.
inline double svfWarp (double frequency, double sampleRate)
{
    const double limited = std::clamp (frequency, 10.0, sampleRate * 0.49);
    return std::tan (3.141592653589793 * limited / sampleRate);
}

inline SvfCoefficients svfCoefficients (double g, double q)
{
    const double k = 1.0 / q;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    return { static_cast<float> (k), static_cast<float> (a1), static_cast<float> (a2), static_cast<float> (g * a2) };
}

inline SvfTaps svfTick (float input, const SvfCoefficients& c, SvfState& state)
{
    const float v3 = input - state.ic2;
    const float v1 = c.a1 * state.ic1 + c.a2 * v3;
    const float v2 = state.ic2 + c.a2 * state.ic1 + c.a3 * v3;
    state.ic1 = 2.0f * v1 - state.ic1;
    state.ic2 = 2.0f * v2 - state.ic2;
    return { v1, v2 };
}

inline float svfOutput (SvfMode mode, float input, float k, SvfTaps taps)
{
    switch (mode)
    {
        case SvfMode::LowPass: return taps.v2;
        case SvfMode::HighPass: return input - k * taps.v1 - taps.v2;
        case SvfMode::BandPass: return k * taps.v1; // Scaled so the peak stays at unity as resonance rises.
        case SvfMode::Notch: return input - k * taps.v1;
    }
    return input;
}

} // namespace Hadronyche
