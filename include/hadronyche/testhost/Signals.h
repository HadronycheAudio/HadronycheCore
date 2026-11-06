#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Test signals and measurements, for the test programs. These decide whether a test passes, so a change here can
// shift what every plug-in's tests measure.

namespace Hadronyche {
namespace TestHost {

constexpr double kSampleRate = 48000.0;

using Samples = std::vector<float>;

struct Tone
{
    double frequency;
    double amplitude;
};

namespace Detail {
constexpr double kPi = 3.14159265358979;
}

inline Samples makeTones (const std::vector<Tone>& tones, size_t count, double sampleRate = kSampleRate)
{
    Samples samples (count, 0.0f);
    for (const auto& tone : tones)
    {
        for (size_t i = 0; i < count; ++i)
            samples[i] += static_cast<float> (tone.amplitude * std::sin (2.0 * Detail::kPi * tone.frequency * static_cast<double> (i) / sampleRate));
    }
    return samples;
}

inline Samples makeNoise (double amplitude, size_t count)
{
    Samples samples (count);
    uint32_t random = 0x9E3779B9u;
    for (auto& sample : samples)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        sample = static_cast<float> (amplitude * (random / 2147483648.0 - 1.0));
    }
    return samples;
}

inline Samples concatenate (const Samples& first, const Samples& second)
{
    Samples result = first;
    result.insert (result.end (), second.begin (), second.end ());
    return result;
}

inline Samples add (const Samples& first, const Samples& second)
{
    Samples result = first;
    for (size_t i = 0; i < result.size () && i < second.size (); ++i)
        result[i] += second[i];
    return result;
}

inline Samples slice (const Samples& samples, size_t from, size_t count)
{
    return Samples (samples.begin () + static_cast<std::ptrdiff_t> (from),
                    samples.begin () + static_cast<std::ptrdiff_t> (std::min (samples.size (), from + count)));
}

inline double rms (const Samples& samples, size_t from = 0)
{
    double energy = 0.0;
    for (size_t i = from; i < samples.size (); ++i)
        energy += static_cast<double> (samples[i]) * samples[i];
    return std::sqrt (energy / static_cast<double> (std::max<size_t> (1, samples.size () - from)));
}

inline double peak (const Samples& samples, size_t from = 0)
{
    double highest = 0.0;
    for (size_t i = from; i < samples.size (); ++i)
        highest = std::max (highest, static_cast<double> (std::abs (samples[i])));
    return highest;
}

inline double db (double value)
{
    return 20.0 * std::log10 (std::max (value, 1e-12));
}

// The level of one frequency, Hann windowed.
inline double toneLevel (const Samples& samples, double hz, size_t from = 0, double sampleRate = kSampleRate)
{
    const size_t count = samples.size () - from;
    double real = 0.0;
    double imaginary = 0.0;
    double windowSum = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (2.0 * Detail::kPi * static_cast<double> (i) / static_cast<double> (count - 1));
        const double phase = 2.0 * Detail::kPi * hz * static_cast<double> (i) / sampleRate;
        real += window * samples[from + i] * std::cos (phase);
        imaginary += window * samples[from + i] * std::sin (phase);
        windowSum += window;
    }
    return 2.0 * std::sqrt (real * real + imaginary * imaginary) / windowSum;
}

inline bool isFinite (const Samples& samples)
{
    return std::all_of (samples.begin (), samples.end (), [] (float sample) { return std::isfinite (sample); });
}

} // namespace TestHost
} // namespace Hadronyche
