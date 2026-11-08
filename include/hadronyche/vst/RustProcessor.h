#pragma once

#include "hadronyche/dsp/Denormals.h"
#include "hadronyche/vst/RustPlugin.h"

#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace Hadronyche {

// An audio component whose signal processing is the plug-in's Rust side: a stereo output, a stereo input and an
// event input when the Rust side says so, 32-bit samples, and the parameters of its table, saved as ParamState.h
// describes. A plug-in's processor adds its class ID. Everything a host sees is decided here, the same way as in
// StereoProcessor, and only the parameters' values, the audio, the transport and the notes cross into Rust.
class RustProcessor : public Steinberg::Vst::AudioEffect
{
public:
    ~RustProcessor () override
    {
        if (_api)
            _api->destroy (_instance);
    }

    Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE
    {
        const Steinberg::tresult result = AudioEffect::initialize (context);
        if (result != Steinberg::kResultOk)
            return result;

        if (hasAudioInput ())
            addAudioInput (STR16 ("Stereo In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);
        if (_api && _api->has_note_input)
            addEventInput (STR16 ("MIDI In"), 16); // Notes are taken from every channel.
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) SMTG_OVERRIDE
    {
        if (state && _api)
            _api->prepare (_instance, processSetup.sampleRate, static_cast<Steinberg::uint32> (processSetup.maxSamplesPerBlock));

        return AudioEffect::setActive (state);
    }

    Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                      Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) SMTG_OVERRIDE
    {
        const bool inputsMatch = hasAudioInput () ? numIns == 1 && inputs[0] == Steinberg::Vst::SpeakerArr::kStereo : numIns == 0;
        if (inputsMatch && numOuts == 1 && outputs[0] == Steinberg::Vst::SpeakerArr::kStereo)
            return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);

        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE
    {
        return symbolicSampleSize == Steinberg::Vst::kSample32 ? Steinberg::kResultTrue : Steinberg::kResultFalse;
    }

    Steinberg::uint32 PLUGIN_API getLatencySamples () SMTG_OVERRIDE { return _api ? _api->latency (_instance) : 0; }

    Steinberg::uint32 PLUGIN_API getTailSamples () SMTG_OVERRIDE
    {
        static_assert (Ffi::INFINITE_TAIL == Steinberg::Vst::kInfiniteTail);
        return _api ? _api->tail (_instance) : 0;
    }

    Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE
    {
        readChanges (data.inputParameterChanges);

        if (data.numOutputs == 0 || data.numSamples == 0)
            return Steinberg::kResultOk;

        Steinberg::Vst::AudioBusBuffers& output = data.outputs[0];
        if (output.numChannels != 2)
        {
            for (Steinberg::int32 c = 0; c < output.numChannels; ++c)
                std::memset (output.channelBuffers32[c], 0, sizeof (float) * static_cast<size_t> (data.numSamples));
            return Steinberg::kResultOk;
        }
        if (!_api)
        {
            std::memset (output.channelBuffers32[0], 0, sizeof (float) * static_cast<size_t> (data.numSamples));
            std::memset (output.channelBuffers32[1], 0, sizeof (float) * static_cast<size_t> (data.numSamples));
            output.silenceFlags = Ffi::SILENT;
            return Steinberg::kResultOk;
        }

        const FlushDenormals flushDenormals;

        Ffi::ProcessBlock block {};
        for (size_t id = 0; id < _values.size (); ++id)
            _snapshot[id] = _values[id];
        block.normalized = _snapshot.data ();
        if (data.numInputs > 0 && data.inputs[0].numChannels == 2)
        {
            block.input_left = data.inputs[0].channelBuffers32[0];
            block.input_right = data.inputs[0].channelBuffers32[1];
        }
        block.output_left = output.channelBuffers32[0];
        block.output_right = output.channelBuffers32[1];
        block.sample_count = static_cast<Steinberg::uint32> (data.numSamples);
        block.transport = readTransport (data.processContext);
        readNotes (data.inputEvents);
        block.notes = _notes.data ();
        block.note_count = static_cast<Steinberg::uint32> (_notes.size ());

        output.silenceFlags = _api->process (_instance, &block);
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
    explicit RustProcessor (const Steinberg::FUID& controllerClass)
    : _api (RustPlugin::get ().api ()), _values (RustPlugin::get ().paramCount ()), _snapshot (RustPlugin::get ().paramCount ())
    {
        setControllerClass (controllerClass);
        for (Steinberg::Vst::ParamID id = 0; id < _values.size (); ++id)
        {
            const ParamSpec& spec = RustPlugin::getSpec (id);
            _values[id] = plainToNormalized (spec, spec.defaultValue);
        }
        _notes.reserve (kMaxNotesPerBlock);
        if (_api)
            _instance = _api->create ();

        // Whatever the host has, and the Rust side takes what it needs. Asking costs nothing.
        processContextRequirements.needTempo ().needTransportState ().needProjectTimeMusic ().needBarPositionMusic ().needTimeSignature ();
    }

    // The parameters alone, for a plug-in that saves more after them.
    bool restoreParams (Steinberg::IBStream* state)
    {
        return RustPlugin::readState (state, [this] (Steinberg::Vst::ParamID id, double value) { _values[id] = value; });
    }

    bool saveParams (Steinberg::IBStream* state)
    {
        std::vector<double> values (_values.size ());
        for (size_t id = 0; id < values.size (); ++id)
            values[id] = _values[id];
        return RustPlugin::writeState (state, values.data ());
    }

private:
    // More in one block than this are left out. A host sends a few at most.
    static constexpr size_t kMaxNotesPerBlock = 1024;

    bool hasAudioInput () const { return _api && _api->has_audio_input; }

    // As ParamStore::readChanges: smoothing is the Rust side's, so only the last value in each block is needed.
    void readChanges (Steinberg::Vst::IParameterChanges* changes)
    {
        if (!changes)
            return;

        const Steinberg::int32 count = changes->getParameterCount ();
        for (Steinberg::int32 i = 0; i < count; ++i)
        {
            Steinberg::Vst::IParamValueQueue* queue = changes->getParameterData (i);
            if (!queue || queue->getParameterId () >= _values.size () || queue->getPointCount () == 0)
                continue;

            Steinberg::int32 offset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint (queue->getPointCount () - 1, offset, value) == Steinberg::kResultTrue)
                _values[queue->getParameterId ()] = value;
        }
    }

    static Ffi::Transport readTransport (const Steinberg::Vst::ProcessContext* context)
    {
        using Steinberg::Vst::ProcessContext;

        Ffi::Transport transport {};
        if (!context)
            return transport;

        if ((context->state & ProcessContext::kPlaying) != 0)
            transport.flags |= Ffi::TRANSPORT_PLAYING;
        if ((context->state & ProcessContext::kTempoValid) != 0)
        {
            transport.flags |= Ffi::TRANSPORT_TEMPO_VALID;
            transport.tempo = context->tempo;
        }
        if ((context->state & ProcessContext::kProjectTimeMusicValid) != 0)
        {
            transport.flags |= Ffi::TRANSPORT_PROJECT_TIME_MUSIC_VALID;
            transport.project_time_music = context->projectTimeMusic;
        }
        if ((context->state & ProcessContext::kBarPositionValid) != 0)
        {
            transport.flags |= Ffi::TRANSPORT_BAR_POSITION_VALID;
            transport.bar_position_music = context->barPositionMusic;
        }
        if ((context->state & ProcessContext::kTimeSigValid) != 0)
        {
            transport.flags |= Ffi::TRANSPORT_TIME_SIGNATURE_VALID;
            transport.time_signature_numerator = context->timeSigNumerator;
            transport.time_signature_denominator = context->timeSigDenominator;
        }
        return transport;
    }

    // Into _notes, which was reserved for them, so that nothing allocates here.
    void readNotes (Steinberg::Vst::IEventList* events)
    {
        using Steinberg::Vst::Event;

        _notes.clear ();
        if (!events)
            return;

        const Steinberg::int32 count = events->getEventCount ();
        for (Steinberg::int32 i = 0; i < count && _notes.size () < kMaxNotesPerBlock; ++i)
        {
            Event event {};
            if (events->getEvent (i, event) != Steinberg::kResultOk)
                continue;

            Ffi::NoteEvent note {};
            note.sample_offset = event.sampleOffset;
            if (event.type == Event::kNoteOnEvent)
            {
                note.kind = Ffi::NoteKind::On;
                note.pitch = event.noteOn.pitch;
                note.channel = event.noteOn.channel;
                note.velocity = event.noteOn.velocity;
                note.tuning = event.noteOn.tuning;
                note.note_id = event.noteOn.noteId;
            }
            else if (event.type == Event::kNoteOffEvent)
            {
                note.kind = Ffi::NoteKind::Off;
                note.pitch = event.noteOff.pitch;
                note.channel = event.noteOff.channel;
                note.velocity = event.noteOff.velocity;
                note.tuning = event.noteOff.tuning;
                note.note_id = event.noteOff.noteId;
            }
            else
                continue;
            _notes.push_back (note);
        }
    }

    const Ffi::PluginApi* _api;
    void* _instance { nullptr };

    // Written from the host's threads and read on the audio thread, as ParamStore's.
    std::vector<std::atomic<double>> _values;
    std::vector<double> _snapshot;
    std::vector<Ffi::NoteEvent> _notes;
};

} // namespace Hadronyche
