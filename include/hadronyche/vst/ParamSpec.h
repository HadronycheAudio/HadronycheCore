#pragma once

#include <algorithm>
#include <cmath>

namespace Hadronyche {

// Each plug-in's table of these is the one definition of its parameters. Hosts store values normalized to
// 0..1, so changing the range or curve of an existing parameter silently changes saved projects.
//
// This header doesn't need the SDK, because a plug-in's tests include its parameters without linking it.

enum class Curve
{
    Linear,
    Log,
    Squared, // Can reach zero, unlike Log, and still gives short times more of the knob's travel.
    List,
    Toggle,
    Integer
};

struct ParamSpec
{
    const char* title;
    const char* units;
    double min;
    double max;
    double defaultValue; // In plain units, like min and max.
    Curve curve;
    int precision;
    const char* const* labels { nullptr }; // Curve::List only.
    int labelCount { 0 };
    bool isSilentAtMin { false }; // Shows the bottom of its range as "-inf", for Volume controls.
    bool isAutomatable { true };
};

// Conversions between the host's 0..1 values and the units in ParamSpec.
inline double normalizedToPlain (const ParamSpec& spec, double normalized)
{
    const double n = std::clamp (normalized, 0.0, 1.0);
    switch (spec.curve)
    {
        case Curve::Linear: return spec.min + (spec.max - spec.min) * n;
        case Curve::Log: return spec.min * std::pow (spec.max / spec.min, n);
        case Curve::Squared: return spec.min + (spec.max - spec.min) * n * n;
        case Curve::List: return std::round (n * (spec.labelCount - 1));
        case Curve::Toggle: return n >= 0.5 ? 1.0 : 0.0;
        case Curve::Integer: return std::round (spec.min + (spec.max - spec.min) * n);
    }
    return spec.min;
}

inline double plainToNormalized (const ParamSpec& spec, double plain)
{
    const double p = std::clamp (plain, spec.min, spec.max);
    switch (spec.curve)
    {
        case Curve::Log: return std::log (p / spec.min) / std::log (spec.max / spec.min);
        case Curve::Squared: return std::sqrt ((p - spec.min) / (spec.max - spec.min));
        case Curve::Linear:
        case Curve::List:
        case Curve::Toggle:
        case Curve::Integer: return (p - spec.min) / (spec.max - spec.min);
    }
    return 0.0;
}

} // namespace Hadronyche
