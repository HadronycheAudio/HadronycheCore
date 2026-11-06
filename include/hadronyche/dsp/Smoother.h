#pragma once

#include <cmath>

namespace Hadronyche {

// One-pole smoothing, which removes clicks and zipper noise from parameter changes.
//
// It has to stay double precision. In single precision, the steps become too small to register before
// reaching the target, and it stalls about 0.01% short, which leaves a held note out of tune after a glide
// and makes a free running loop the wrong length.
class Smoother
{
public:
    // sampleRate is how often next is called, per second.
    void prepare (double sampleRate, double timeMs)
    {
        _coefficient = std::exp (-1000.0 / (sampleRate * timeMs));
    }

    void setTarget (double target) { _target = target; }

    void snapToTarget () { _current = _target; }

    void snapTo (double value)
    {
        _target = value;
        _current = value;
    }

    double next ()
    {
        _current = _target + (_current - _target) * _coefficient;
        return _current;
    }

    // For single precision signal paths. The result goes through float, so an expression using it stays
    // in float, which a plain next would change.
    float nextFloat () { return static_cast<float> (next ()); }

private:
    double _coefficient { 0.0 };
    double _target { 0.0 };
    double _current { 0.0 };
};

} // namespace Hadronyche
