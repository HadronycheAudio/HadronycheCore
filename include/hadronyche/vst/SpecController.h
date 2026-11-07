#pragma once

#include "hadronyche/vst/ParamSpec.h"
#include "hadronyche/vst/SpecParameter.h"

#include "public.sdk/source/vst/vsteditcontroller.h"

#include <functional>

namespace Hadronyche {

// What a plug-in's reader of its saved parameters gets: the ID and the normalized value of each one. A plug-in's
// own readParamState has this shape, and converts old values on the way.
using ParamValueSink = std::function<void (Steinberg::Vst::ParamID, double)>;
using ParamStateReader = bool (*) (Steinberg::IBStream*, const ParamValueSink&);

// An edit controller whose parameters all come from a plug-in's ParamSpec table, and whose state is the saved
// parameters. A plug-in's controller adds its class ID and its editor, and whatever else it needs.
class SpecController : public Steinberg::Vst::EditController
{
public:
    using SpecGetter = const ParamSpec& (*) (Steinberg::Vst::ParamID);

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE
    {
        const Steinberg::tresult result = EditController::initialize (context);
        if (result != Steinberg::kResultOk)
            return result;

        for (Steinberg::Vst::ParamID id = 0; id < _paramCount; ++id)
            parameters.addParameter (new SpecParameter (id, _getSpec (id)));

        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setComponentState (Steinberg::IBStream* state) SMTG_OVERRIDE
    {
        if (!state)
            return Steinberg::kResultFalse;

        const bool ok = _readState (state, [this] (Steinberg::Vst::ParamID id, double value) { setParamNormalized (id, value); });
        return ok ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }

    Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) SMTG_OVERRIDE
    {
        if (Steinberg::FIDStringsEqual (name, Steinberg::Vst::ViewType::kEditor))
            return createEditor ();

        return nullptr;
    }

protected:
    SpecController (SpecGetter getSpec, Steinberg::Vst::ParamID paramCount, ParamStateReader readState)
    : _getSpec (getSpec), _paramCount (paramCount), _readState (readState)
    {
    }

    // A new editor, which the host releases.
    virtual Steinberg::IPlugView* createEditor () = 0;

private:
    SpecGetter _getSpec;
    Steinberg::Vst::ParamID _paramCount;
    ParamStateReader _readState;
};

} // namespace Hadronyche
