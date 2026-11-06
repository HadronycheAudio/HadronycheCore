#pragma once

#include "hadronyche/vst/ParamSpec.h"

#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstparameters.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Hadronyche {

// A controller's parameter whose range, curve and text all come from its ParamSpec.
class SpecParameter : public Steinberg::Vst::Parameter
{
public:
    SpecParameter (Steinberg::Vst::ParamID id, const ParamSpec& spec)
    : Steinberg::Vst::Parameter (Steinberg::UString128 (spec.title), id, Steinberg::UString128 (spec.units),
                                 plainToNormalized (spec, spec.defaultValue), getStepCount (spec), getFlags (spec)),
      _spec (spec)
    {
    }

    void toString (Steinberg::Vst::ParamValue normalized, Steinberg::Vst::String128 string) const SMTG_OVERRIDE
    {
        const double plain = normalizedToPlain (_spec, normalized);

        char text[64] {};
        if (_spec.curve == Curve::List)
            std::snprintf (text, sizeof (text), "%s", _spec.labels[static_cast<int> (plain)]);
        else if (_spec.curve == Curve::Toggle)
            std::snprintf (text, sizeof (text), "%s", plain > 0.5 ? "On" : "Off");
        else if (_spec.isSilentAtMin && plain <= _spec.min)
            std::snprintf (text, sizeof (text), "-inf");
        else
            std::snprintf (text, sizeof (text), "%.*f", _spec.precision, plain);

        Steinberg::UString (string, str16BufferSize (Steinberg::Vst::String128)).fromAscii (text);
    }

    bool fromString (const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& normalized) const SMTG_OVERRIDE
    {
        char text[128] {};
        Steinberg::UString (const_cast<Steinberg::Vst::TChar*> (string), Steinberg::tstrlen (string)).toAscii (text, sizeof (text));

        if (_spec.curve == Curve::List)
        {
            for (int i = 0; i < _spec.labelCount; ++i)
            {
                if (std::strcmp (text, _spec.labels[i]) == 0)
                {
                    normalized = plainToNormalized (_spec, i);
                    return true;
                }
            }
            return false;
        }

        if (_spec.curve == Curve::Toggle)
        {
            normalized = std::strcmp (text, "On") == 0 ? 1.0 : 0.0;
            return true;
        }

        if (std::strcmp (text, "-inf") == 0)
        {
            normalized = 0.0;
            return _spec.isSilentAtMin;
        }

        double plain = 0.0;
        if (std::sscanf (text, "%lf", &plain) != 1 || !std::isfinite (plain))
            return false;

        normalized = plainToNormalized (_spec, plain);
        return true;
    }

    Steinberg::Vst::ParamValue toPlain (Steinberg::Vst::ParamValue normalized) const SMTG_OVERRIDE
    {
        return normalizedToPlain (_spec, normalized);
    }

    Steinberg::Vst::ParamValue toNormalized (Steinberg::Vst::ParamValue plain) const SMTG_OVERRIDE
    {
        return plainToNormalized (_spec, plain);
    }

private:
    static Steinberg::int32 getStepCount (const ParamSpec& spec)
    {
        if (spec.curve == Curve::List)
            return spec.labelCount - 1;
        if (spec.curve == Curve::Integer)
            return static_cast<Steinberg::int32> (spec.max - spec.min);
        return spec.curve == Curve::Toggle ? 1 : 0;
    }

    static Steinberg::int32 getFlags (const ParamSpec& spec)
    {
        Steinberg::int32 flags = spec.isAutomatable ? Steinberg::Vst::ParameterInfo::kCanAutomate : 0;
        if (spec.curve == Curve::List)
            flags |= Steinberg::Vst::ParameterInfo::kIsList;
        return flags;
    }

    const ParamSpec& _spec;
};

} // namespace Hadronyche
