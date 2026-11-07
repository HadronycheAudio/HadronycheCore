#pragma once

#include "hadronyche/vst/ParamSpec.h"

#include "pluginterfaces/vst/ivsthostapplication.h"

namespace Hadronyche {
namespace Lv2 {

class HostContext : public Steinberg::Vst::IHostApplication
{
public:
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** object) override
    {
        if (Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::Vst::IHostApplication::iid) || Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::FUnknown::iid))
        {
            *object = static_cast<Steinberg::Vst::IHostApplication*> (this);
            return Steinberg::kResultOk;
        }
        *object = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef () override { return 1; }
    Steinberg::uint32 PLUGIN_API release () override { return 1; }

    Steinberg::tresult PLUGIN_API getName (Steinberg::Vst::String128 name) override
    {
        name[0] = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API createInstance (Steinberg::TUID, Steinberg::TUID, void** object) override
    {
        *object = nullptr;
        return Steinberg::kResultFalse;
    }
};

// The SDK's module, which an instance and a UI each hold open.
bool enterModule ();
void exitModule ();

// A float can't hold every bottom of range, such as 0.001, and the nearest one is a little above it. This keeps a
// host's minimum and maximum the same as a VST3 host's 0 and 1.
inline double controlToNormalized (const ParamSpec& spec, float value)
{
    if (value <= static_cast<float> (spec.min))
        return 0.0;
    if (value >= static_cast<float> (spec.max))
        return 1.0;
    return plainToNormalized (spec, value);
}

} // namespace Lv2
} // namespace Hadronyche
