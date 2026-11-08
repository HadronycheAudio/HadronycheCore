#pragma once

#include "hadronyche/ffi/Plugin.h"
#include "hadronyche/vst/ParamSpec.h"
#include "hadronyche/vst/ParamState.h"
#include "hadronyche/vst/SpecController.h"

#include <algorithm>
#include <vector>

namespace Hadronyche {

// The plug-in's Rust side, reached through the one symbol that its export_plugin! defines, with its parameter table
// read into ParamSpecs, so that the controller, the panel and the LV2 wrapper take it the same way as a C++ table.
// Built once, on first use.
class RustPlugin
{
public:
    static const RustPlugin& get ()
    {
        static const RustPlugin plugin;
        return plugin;
    }

    // Null when the Rust side was built against another Plugin.h than this one, and then the plug-in has no
    // parameters and its processor stays silent, which the plug-in's tests see at once.
    const Ffi::PluginApi* api () const { return _api; }
    Steinberg::Vst::ParamID paramCount () const { return static_cast<Steinberg::Vst::ParamID> (_specs.size ()); }
    const ParamSpec& spec (Steinberg::Vst::ParamID id) const { return _specs[id]; }

    // The same shapes as a plug-in's own getParamSpec and readParamState, for SpecController, PanelEditor and
    // PluginInfo. The state is ParamState.h's format, with the Rust side's version, and its older values are
    // converted by the Rust side.
    static const ParamSpec& getSpec (Steinberg::Vst::ParamID id) { return get ().spec (id); }

    static bool readState (Steinberg::IBStream* stream, const ParamValueSink& onValue)
    {
        const RustPlugin& plugin = get ();
        if (!plugin._api)
            return false;

        const Ffi::PluginApi& api = *plugin._api;
        return readParamState (stream, api.state_version, plugin.paramCount (),
                               [&api, &onValue] (Steinberg::Vst::ParamID id, double value, Steinberg::int32 version) {
                                   onValue (id, std::clamp (api.migrate_param (id, value, version), 0.0, 1.0));
                               });
    }

    static bool writeState (Steinberg::IBStream* stream, const double* normalized)
    {
        const RustPlugin& plugin = get ();
        if (!plugin._api)
            return false;

        return writeParamState (stream, plugin._api->state_version, normalized, plugin.paramCount ());
    }

private:
    RustPlugin ()
    {
        const Ffi::PluginApi* api = hadronyche_plugin ();
        if (!api || api->abi_version != Ffi::ABI_VERSION)
            return;

        _api = api;
        // Sized once, because the specs point into it.
        _labels.resize (api->param_count);
        _specs.reserve (api->param_count);
        for (Steinberg::uint32 id = 0; id < api->param_count; ++id)
        {
            Ffi::ParamInfo info {};
            api->param_spec (id, &info);

            std::vector<const char*>& labels = _labels[id];
            for (Steinberg::uint32 index = 0; index < info.label_count; ++index)
                labels.push_back (api->param_label (id, index));

            ParamSpec spec { info.title, info.units, info.min, info.max, info.default_value, toCurve (info.curve), info.precision };
            spec.labels = labels.empty () ? nullptr : labels.data ();
            spec.labelCount = static_cast<int> (labels.size ());
            spec.isSilentAtMin = info.is_silent_at_min;
            spec.isAutomatable = info.is_automatable;
            _specs.push_back (spec);
        }
    }

    // The Rust side's curves are numbered as these are, which Plugin.h fixes.
    static Curve toCurve (Ffi::Curve curve)
    {
        static_assert (static_cast<int> (Ffi::Curve::Linear) == static_cast<int> (Curve::Linear));
        static_assert (static_cast<int> (Ffi::Curve::Log) == static_cast<int> (Curve::Log));
        static_assert (static_cast<int> (Ffi::Curve::Squared) == static_cast<int> (Curve::Squared));
        static_assert (static_cast<int> (Ffi::Curve::List) == static_cast<int> (Curve::List));
        static_assert (static_cast<int> (Ffi::Curve::Toggle) == static_cast<int> (Curve::Toggle));
        static_assert (static_cast<int> (Ffi::Curve::Integer) == static_cast<int> (Curve::Integer));
        return static_cast<Curve> (static_cast<int> (curve));
    }

    const Ffi::PluginApi* _api { nullptr };
    std::vector<ParamSpec> _specs;
    std::vector<std::vector<const char*>> _labels;
};

} // namespace Hadronyche
