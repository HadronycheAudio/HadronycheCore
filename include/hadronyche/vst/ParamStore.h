#pragma once

#include "hadronyche/vst/ParamSpec.h"

#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace Hadronyche {

// A processor's normalized parameter values, written from the host's threads and read on the audio thread.
template <size_t Count>
class ParamStore
{
public:
    using SpecGetter = const ParamSpec& (*) (Steinberg::Vst::ParamID);
    using Values = std::array<double, Count>;

    explicit ParamStore (SpecGetter getSpec) : _getSpec (getSpec)
    {
        for (Steinberg::Vst::ParamID id = 0; id < Count; ++id)
        {
            const auto& spec = _getSpec (id);
            _values[id] = plainToNormalized (spec, spec.defaultValue);
        }
    }

    std::atomic<double>& operator[] (Steinberg::Vst::ParamID id) { return _values[id]; }

    double plain (Steinberg::Vst::ParamID id) const { return normalizedToPlain (_getSpec (id), _values[id]); }

    // Safe on the audio thread. Smoothing covers the rest, so only the last value in each block is needed.
    void readChanges (Steinberg::Vst::IParameterChanges* changes)
    {
        if (!changes)
            return;

        const Steinberg::int32 count = changes->getParameterCount ();
        for (Steinberg::int32 i = 0; i < count; ++i)
        {
            auto* queue = changes->getParameterData (i);
            if (!queue || queue->getParameterId () >= Count || queue->getPointCount () == 0)
                continue;

            Steinberg::int32 offset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint (queue->getPointCount () - 1, offset, value) == Steinberg::kResultTrue)
                _values[queue->getParameterId ()] = value;
        }
    }

    Values snapshot () const
    {
        Values values {};
        for (size_t id = 0; id < Count; ++id)
            values[id] = _values[id];
        return values;
    }

private:
    SpecGetter _getSpec;
    std::array<std::atomic<double>, Count> _values;
};

} // namespace Hadronyche
