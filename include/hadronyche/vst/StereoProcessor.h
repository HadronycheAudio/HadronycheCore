#pragma once

#include "hadronyche/dsp/Denormals.h"
#include "hadronyche/vst/ParamStore.h"
#include "hadronyche/vst/SpecController.h"

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace Hadronyche {

// An audio component with a stereo output, a stereo input or none, 32-bit samples, and the parameters of a ParamSpec
// table, saved as ParamState.h describes. A plug-in's processor adds its class ID, its DSP in prepareDsp and
// renderBlock, and whatever else it needs, such as an event input or a longer state.
template <size_t ParamCount>
class StereoProcessor : public Steinberg::Vst::AudioEffect
{
public:
    using Params = ParamStore<ParamCount>;
    using ParamStateWriter = bool (*) (Steinberg::IBStream*, const typename Params::Values&);

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE
    {
        const Steinberg::tresult result = AudioEffect::initialize (context);
        if (result != Steinberg::kResultOk)
            return result;

        if (_hasAudioInput)
            addAudioInput (STR16 ("Stereo In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) SMTG_OVERRIDE
    {
        if (state)
            prepareDsp ();

        return AudioEffect::setActive (state);
    }

    Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                      Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE
    {
        const bool inputsMatch = _hasAudioInput ? numIns == 1 && inputs[0] == Steinberg::Vst::SpeakerArr::kStereo : numIns == 0;
        if (inputsMatch && numOuts == 1 && outputs[0] == Steinberg::Vst::SpeakerArr::kStereo)
            return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);

        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE
    {
        return symbolicSampleSize == Steinberg::Vst::kSample32 ? Steinberg::kResultTrue : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE
    {
        _params.readChanges (data.inputParameterChanges);

        if (data.numOutputs == 0 || data.numSamples == 0)
            return Steinberg::kResultOk;

        auto& output = data.outputs[0];
        if (output.numChannels != 2)
        {
            for (Steinberg::int32 c = 0; c < output.numChannels; ++c)
                std::memset (output.channelBuffers32[c], 0, sizeof (float) * static_cast<size_t> (data.numSamples));
            return Steinberg::kResultOk;
        }

        const FlushDenormals flushDenormals;
        renderBlock (data, output);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE
    {
        if (!state)
            return Steinberg::kResultFalse;

        return restoreParams (state) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE
    {
        if (!state)
            return Steinberg::kResultFalse;

        return saveParams (state) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }

protected:
    StereoProcessor (const Steinberg::FUID& controllerClass, bool hasAudioInput, typename Params::SpecGetter getSpec,
                     ParamStateReader readState, ParamStateWriter writeState)
    : _params (getSpec), _hasAudioInput (hasAudioInput), _readState (readState), _writeState (writeState)
    {
        setControllerClass (controllerClass);
    }

    // Before the first block, and again after every activation, with processSetup set.
    virtual void prepareDsp () = 0;

    // One block, with the parameter changes already taken and denormals already flushed. Its output has two
    // channels, and the plug-in sets its silence flags.
    virtual void renderBlock (Steinberg::Vst::ProcessData& data, Steinberg::Vst::AudioBusBuffers& output) = 0;

    // The parameters alone, for a plug-in that saves more after them.
    bool restoreParams (Steinberg::IBStream* state)
    {
        return _readState (state, [this] (Steinberg::Vst::ParamID id, double value) { _params[id] = value; });
    }

    bool saveParams (Steinberg::IBStream* state) { return _writeState (state, _params.snapshot ()); }

    Params _params;

private:
    bool _hasAudioInput;
    ParamStateReader _readState;
    ParamStateWriter _writeState;
};

} // namespace Hadronyche
