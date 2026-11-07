// Usage: <program> <VST3 .so> <LV2 bundle folder> [scenario name contains]
//
// Plays the same audio, notes, control changes, transport and saved state through a plug-in's VST3 and its LV2, and
// compares their output bit for bit. Both run the same processor, so any difference is the LV2 wrapper's fault. It only
// uses the two formats' interfaces, and it reads the LV2's ports from its Turtle, as a host does.
//
// A control port holds a float, so the VST3 side gets the normalized value of the same float. "Float steps" below
// measures what that costs against a VST3 given unrounded values. It is reported, and can't fail.
//
// It also checks that the Turtle agrees with the VST3 about every parameter, that run () doesn't allocate, and that
// a wrong note time or control value would be noticed, so that a pass means something.

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Steinberg;
namespace fs = std::filesystem;

DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IHostApplication)

// Counting allocations here catches those inside the plug-in too, because this program exports these functions and
// the plug-in looks them up by name. Each thread counts its own, because only the one calling run () has to keep from
// allocating, and a plug-in's other threads may be allocating meanwhile.
extern "C" void* __libc_malloc (size_t);
extern "C" void* __libc_calloc (size_t, size_t);
extern "C" void* __libc_realloc (void*, size_t);

namespace {
thread_local long gAllocations = 0;
}

extern "C" void* malloc (size_t size)
{
    ++gAllocations;
    return __libc_malloc (size);
}

extern "C" void* calloc (size_t count, size_t size)
{
    ++gAllocations;
    return __libc_calloc (count, size);
}

extern "C" void* realloc (void* memory, size_t size)
{
    ++gAllocations;
    return __libc_realloc (memory, size);
}

namespace {

constexpr int kMaxBlock = 4096;
constexpr double kPi = 3.14159265358979;

IPluginFactory* gFactory = nullptr;
TUID gComponentId;
TUID gControllerId;
const LV2_Descriptor* gDescriptor = nullptr;

#define LV2COMPARE_NO_REFCOUNT                                                                \
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }  \
    uint32 PLUGIN_API addRef () override { return 1; }                                        \
    uint32 PLUGIN_API release () override { return 1; }

class Host : public Vst::IHostApplication
{
public:
    LV2COMPARE_NO_REFCOUNT

    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        name[0] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (TUID, TUID, void** object) override
    {
        *object = nullptr;
        return kResultFalse;
    }
};

Host gHost;

class ValueQueue : public Vst::IParamValueQueue
{
public:
    LV2COMPARE_NO_REFCOUNT

    explicit ValueQueue (Vst::ParamID id) : _id (id) {}

    void add (int32 offset, double value) { _points.push_back ({ offset, value }); }

    Vst::ParamID PLUGIN_API getParameterId () override { return _id; }
    int32 PLUGIN_API getPointCount () override { return static_cast<int32> (_points.size ()); }

    tresult PLUGIN_API getPoint (int32 index, int32& offset, Vst::ParamValue& value) override
    {
        if (index < 0 || index >= getPointCount ())
            return kResultFalse;
        offset = _points[static_cast<size_t> (index)].first;
        value = _points[static_cast<size_t> (index)].second;
        return kResultTrue;
    }

    tresult PLUGIN_API addPoint (int32, Vst::ParamValue, int32&) override { return kResultFalse; }

private:
    Vst::ParamID _id;
    std::vector<std::pair<int32, double>> _points;
};

class ParameterChanges : public Vst::IParameterChanges
{
public:
    LV2COMPARE_NO_REFCOUNT

    int32 PLUGIN_API getParameterCount () override { return static_cast<int32> (queues.size ()); }

    Vst::IParamValueQueue* PLUGIN_API getParameterData (int32 index) override
    {
        return index >= 0 && index < getParameterCount () ? &queues[static_cast<size_t> (index)] : nullptr;
    }

    Vst::IParamValueQueue* PLUGIN_API addParameterData (const Vst::ParamID&, int32&) override { return nullptr; }

    std::vector<ValueQueue> queues;
};

class EventList : public Vst::IEventList
{
public:
    LV2COMPARE_NO_REFCOUNT

    int32 PLUGIN_API getEventCount () override { return static_cast<int32> (events.size ()); }

    tresult PLUGIN_API getEvent (int32 index, Vst::Event& event) override
    {
        if (index < 0 || index >= getEventCount ())
            return kResultFalse;
        event = events[static_cast<size_t> (index)];
        return kResultOk;
    }

    tresult PLUGIN_API addEvent (Vst::Event&) override { return kResultFalse; }

    std::vector<Vst::Event> events;
};

struct Note
{
    size_t position;
    uint8_t status; // MIDI's, with the channel.
    uint8_t pitch;
    uint8_t velocity;
};

// What the Turtle says about the ports.
struct Port
{
    uint32_t index { 0 };
    std::string symbol;
    std::string name;
    double minimum { 0.0 };
    double maximum { 0.0 };
    double defaultValue { 0.0 };
};

struct Turtle
{
    std::map<std::string, uint32_t> special; // Audio, events and latency, by symbol.
    std::vector<Port> controls;              // Inputs only, in index order.
    bool supportsTransport { false };
    bool supportsMidi { false };
    int controlDesignations { 0 };
};

Turtle gTurtle;
bool gHasAudioInput = false;
bool gHasMidi = false;

// Junk where a plug-in should have written, so that output it leaves alone can't match by luck.
constexpr float kUnwritten = -12345.0f;

struct Transport
{
    double tempo;
    double beat;
    bool isPlaying;
};

class Driver
{
public:
    virtual ~Driver () = default;

    // A control's final value for the next block, in plain units, as an LV2 port holds it.
    virtual void setControl (Vst::ParamID id, float plain) = 0;
    virtual void setTransport (const Transport& transport) = 0;
    virtual void render (int count, const std::vector<Note>& notes, float* inLeft, float* inRight, float* left, float* right) = 0;
    virtual void reactivate () = 0;
    virtual std::vector<char> getState () = 0;
    virtual bool setState (const std::vector<char>& bytes) = 0;
};

// Controller used only for its conversions, so that the wrapper's own are checked against it.
class Vst3Driver : public Driver
{
public:
    explicit Vst3Driver (double sampleRate) : _sampleRate (sampleRate)
    {
        gFactory->createInstance (gComponentId, Vst::IComponent::iid, reinterpret_cast<void**> (&_component));
        _component->initialize (&gHost);
        _component->queryInterface (Vst::IAudioProcessor::iid, reinterpret_cast<void**> (&processor));
        gFactory->createInstance (gControllerId, Vst::IEditController::iid, reinterpret_cast<void**> (&controller));
        controller->initialize (&gHost);

        Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, kMaxBlock, sampleRate };
        processor->setupProcessing (setup);
        _hasInput = _component->getBusCount (Vst::kAudio, Vst::kInput) > 0;
        _hasEvents = _component->getBusCount (Vst::kEvent, Vst::kInput) > 0;
        if (_hasInput)
            _component->activateBus (Vst::kAudio, Vst::kInput, 0, true);
        _component->activateBus (Vst::kAudio, Vst::kOutput, 0, true);
        if (_hasEvents)
            _component->activateBus (Vst::kEvent, Vst::kInput, 0, true);
        _component->setActive (true);
        processor->setProcessing (true);
    }

    ~Vst3Driver () override
    {
        processor->setProcessing (false);
        _component->setActive (false);
        controller->terminate ();
        controller->release ();
        processor->release ();
        _component->terminate ();
        _component->release ();
    }

    bool hasInput () const { return _hasInput; }
    bool hasEvents () const { return _hasEvents; }

    void setControl (Vst::ParamID id, float plain) override { setNormalized (id, normalizedForControl (id, plain)); }

    void setNormalized (Vst::ParamID id, double normalized)
    {
        ValueQueue queue (id);
        queue.add (0, normalized);
        _changes.queues.push_back (queue);
    }

    // Only the last point counts, as a control port's one value per block assumes.
    void setRamp (Vst::ParamID id, double first, double last)
    {
        ValueQueue queue (id);
        queue.add (0, first);
        queue.add (kMaxBlock - 1, last);
        _changes.queues.push_back (queue);
    }

    double normalizedForControl (Vst::ParamID id, float plain)
    {
        if (plain <= static_cast<float> (controller->normalizedParamToPlain (id, 0.0)))
            return 0.0;
        if (plain >= static_cast<float> (controller->normalizedParamToPlain (id, 1.0)))
            return 1.0;
        return controller->plainParamToNormalized (id, plain);
    }

    void setTransport (const Transport& transport) override
    {
        _context.state = Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kTempoValid
                         | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kBarPositionValid;
        if (transport.isPlaying)
            _context.state |= Vst::ProcessContext::kPlaying;
        _context.sampleRate = _sampleRate;
        _context.tempo = transport.tempo;
        _context.timeSigNumerator = 4;
        _context.timeSigDenominator = 4;
        _context.projectTimeMusic = transport.beat;
        _context.barPositionMusic = std::floor (transport.beat / 4.0) * 4.0;
        _hasContext = true;
    }

    void render (int count, const std::vector<Note>& notes, float* inLeft, float* inRight, float* left, float* right) override
    {
        _events.events.clear ();
        for (const auto& note : notes)
        {
            Vst::Event event {};
            event.sampleOffset = static_cast<int32> (note.position);
            const float velocity = static_cast<float> (note.velocity) / 127.0f;
            if ((note.status & 0xF0) == 0x90)
            {
                event.type = Vst::Event::kNoteOnEvent;
                event.noteOn.channel = note.status & 0x0F;
                event.noteOn.pitch = note.pitch;
                event.noteOn.velocity = velocity;
                event.noteOn.noteId = -1;
            }
            else if ((note.status & 0xF0) == 0x80)
            {
                event.type = Vst::Event::kNoteOffEvent;
                event.noteOff.channel = note.status & 0x0F;
                event.noteOff.pitch = note.pitch;
                event.noteOff.velocity = velocity;
                event.noteOff.noteId = -1;
            }
            else
            {
                continue; // A VST3 host doesn't send other messages as events.
            }
            _events.events.push_back (event);
        }

        float* inputs[2] = { inLeft, inRight };
        float* outputs[2] = { left, right };
        Vst::AudioBusBuffers input {};
        input.numChannels = 2;
        input.channelBuffers32 = inputs;
        Vst::AudioBusBuffers output {};
        output.numChannels = 2;
        output.channelBuffers32 = outputs;

        Vst::ProcessData data {};
        data.processMode = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples = count;
        data.numInputs = _hasInput ? 1 : 0;
        data.inputs = _hasInput ? &input : nullptr;
        data.numOutputs = 1;
        data.outputs = &output;
        data.inputParameterChanges = _changes.queues.empty () ? nullptr : &_changes;
        data.inputEvents = _hasEvents ? &_events : nullptr;
        data.processContext = _hasContext ? &_context : nullptr;
        processor->process (data);
        _changes.queues.clear ();

        if (_hasContext && (_context.state & Vst::ProcessContext::kPlaying) != 0)
        {
            _context.projectTimeMusic += count * _context.tempo / (60.0 * _sampleRate);
            _context.projectTimeSamples += count;
            _context.barPositionMusic = std::floor (_context.projectTimeMusic / 4.0) * 4.0;
        }
    }

    void reactivate () override
    {
        processor->setProcessing (false);
        _component->setActive (false);
        _component->setActive (true);
        processor->setProcessing (true);
    }

    std::vector<char> getState () override
    {
        MemoryStream stream;
        _component->getState (&stream);
        return std::vector<char> (stream.getData (), stream.getData () + stream.getSize ());
    }

    bool setState (const std::vector<char>& bytes) override
    {
        MemoryStream stream;
        int32 written = 0;
        int64 position = 0;
        if (!bytes.empty ())
            stream.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
        stream.seek (0, IBStream::kIBSeekSet, &position);
        return _component->setState (&stream) == kResultOk;
    }

    Vst::IAudioProcessor* processor { nullptr };
    Vst::IEditController* controller { nullptr };

private:
    double _sampleRate;
    bool _hasInput { false };
    bool _hasEvents { false };
    bool _hasContext { false };
    Vst::ProcessContext _context {};
    Vst::IComponent* _component { nullptr };
    ParameterChanges _changes;
    EventList _events;
};

// As little as a host can get away with.
class Urids
{
public:
    Urids ()
    {
        _map.handle = this;
        _map.map = [] (LV2_URID_Map_Handle handle, const char* uri) -> LV2_URID {
            auto& known = static_cast<Urids*> (handle)->_known;
            const auto found = known.find (uri);
            if (found != known.end ())
                return found->second;
            const auto urid = static_cast<LV2_URID> (known.size () + 1);
            known[uri] = urid;
            return urid;
        };
    }

    LV2_URID_Map* map () { return &_map; }
    LV2_URID get (const char* uri) { return _map.map (_map.handle, uri); }

private:
    LV2_URID_Map _map {};
    std::map<std::string, LV2_URID> _known;
};

Urids gUrids;

class Lv2Driver : public Driver
{
public:
    explicit Lv2Driver (double sampleRate)
    {
        _maxBlock = kMaxBlock;
        const LV2_Options_Option options[] = {
            { LV2_OPTIONS_INSTANCE, 0, gUrids.get (LV2_BUF_SIZE__maxBlockLength), sizeof (int32_t), gUrids.get (LV2_ATOM__Int), &_maxBlock },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },
        };
        const LV2_Feature mapFeature { LV2_URID__map, gUrids.map () };
        const LV2_Feature optionsFeature { LV2_OPTIONS__options, const_cast<LV2_Options_Option*> (options) };
        const LV2_Feature boundedFeature { LV2_BUF_SIZE__boundedBlockLength, nullptr };
        const LV2_Feature* const features[] = { &mapFeature, &optionsFeature, &boundedFeature, nullptr };

        _instance = gDescriptor->instantiate (gDescriptor, sampleRate, "", features);
        if (!_instance)
            return;

        _controls.resize (gTurtle.controls.size ());
        for (size_t i = 0; i < gTurtle.controls.size (); ++i)
        {
            _controls[i] = static_cast<float> (gTurtle.controls[i].defaultValue);
            gDescriptor->connect_port (_instance, gTurtle.controls[i].index, &_controls[i]);
        }

        _eventBuffer.resize (64 * 1024);
        const auto events = gTurtle.special.find ("events");
        if (events != gTurtle.special.end ())
            gDescriptor->connect_port (_instance, events->second, _eventBuffer.data ());
        const auto latency = gTurtle.special.find ("latency");
        if (latency != gTurtle.special.end ())
            gDescriptor->connect_port (_instance, latency->second, &latencyPort);

        // Message ports, with nothing from a panel, so that run ()'s side of them is counted for allocations too.
        const auto messageInput = gTurtle.special.find ("ui_in");
        const auto messageOutput = gTurtle.special.find ("ui_out");
        if (messageInput != gTurtle.special.end () && messageOutput != gTurtle.special.end ())
        {
            _messageInput.resize (sizeof (LV2_Atom_Sequence));
            _messageOutput.resize (64 * 1024);
            auto* input = reinterpret_cast<LV2_Atom_Sequence*> (_messageInput.data ());
            input->atom.type = gUrids.get (LV2_ATOM__Sequence);
            input->atom.size = sizeof (LV2_Atom_Sequence_Body);
            gDescriptor->connect_port (_instance, messageInput->second, _messageInput.data ());
            gDescriptor->connect_port (_instance, messageOutput->second, _messageOutput.data ());
        }

        lv2_atom_forge_init (&_forge, gUrids.map ());
        gDescriptor->activate (_instance);
    }

    ~Lv2Driver () override
    {
        if (!_instance)
            return;
        gDescriptor->deactivate (_instance);
        gDescriptor->cleanup (_instance);
    }

    bool isValid () const { return _instance != nullptr; }

    void setControl (Vst::ParamID id, float plain) override { _controls[id] = plain; }

    void setTransport (const Transport& transport) override
    {
        _pendingTransport = transport;
        _hasPendingTransport = true;
    }

    void render (int count, const std::vector<Note>& notes, float* inLeft, float* inRight, float* left, float* right) override
    {
        // Ports can move between blocks, so they get connected every time, as some hosts do.
        if (gHasAudioInput)
        {
            gDescriptor->connect_port (_instance, gTurtle.special.at ("in_left"), inLeft);
            gDescriptor->connect_port (_instance, gTurtle.special.at ("in_right"), inRight);
        }
        gDescriptor->connect_port (_instance, gTurtle.special.at ("out_left"), left);
        gDescriptor->connect_port (_instance, gTurtle.special.at ("out_right"), right);

        forgeEvents (notes);

        // A host gives the output port its room, as a chunk, before each run ().
        auto* messages = reinterpret_cast<LV2_Atom_Sequence*> (_messageOutput.data ());
        if (messages)
        {
            messages->atom.type = gUrids.get (LV2_ATOM__Chunk);
            messages->atom.size = static_cast<uint32_t> (_messageOutput.size () - sizeof (LV2_Atom));
        }

        const long before = gAllocations;
        gDescriptor->run (_instance, static_cast<uint32_t> (count));
        allocationsInRun += gAllocations - before;

        if (messages)
        {
            const bool isSequence = messages->atom.type == gUrids.get (LV2_ATOM__Sequence)
                                    && messages->atom.size >= sizeof (LV2_Atom_Sequence_Body)
                                    && messages->atom.size <= _messageOutput.size () - sizeof (LV2_Atom);
            if (!isSequence)
                ++badMessageOutputs;
            else
            {
                // Each event has to be a whole message object, where the sequence's own stepping finds it.
                const LV2_URID messageType = gUrids.get ((std::string (gDescriptor->URI) + "#message").c_str ());
                LV2_ATOM_SEQUENCE_FOREACH (messages, event)
                {
                    ++messagesOut;
                    const auto* object = reinterpret_cast<const LV2_Atom_Object*> (&event->body);
                    const uint32_t end = static_cast<uint32_t> (reinterpret_cast<const uint8_t*> (event) - reinterpret_cast<const uint8_t*> (&messages->body))
                                         + static_cast<uint32_t> (sizeof (LV2_Atom_Event)) + event->body.size;
                    if (event->body.type != gUrids.get (LV2_ATOM__Object) || event->body.size < sizeof (LV2_Atom_Object_Body)
                        || object->body.otype != messageType || end > messages->atom.size)
                        ++badMessageOutputs;
                }
            }
        }
    }

    void reactivate () override
    {
        gDescriptor->deactivate (_instance);
        gDescriptor->activate (_instance);
    }

    std::vector<char> getState () override
    {
        const auto* state = static_cast<const LV2_State_Interface*> (gDescriptor->extension_data (LV2_STATE__interface));
        std::vector<char> bytes;
        const auto status = state->save (_instance, [] (LV2_State_Handle handle, uint32_t, const void* value, size_t size, uint32_t type, uint32_t) -> LV2_State_Status {
            if (type != gUrids.get (LV2_ATOM__Chunk))
                return LV2_STATE_ERR_BAD_TYPE;
            const auto* start = static_cast<const char*> (value);
            static_cast<std::vector<char>*> (handle)->assign (start, start + size);
            return LV2_STATE_SUCCESS;
        }, &bytes, 0, nullptr);
        return status == LV2_STATE_SUCCESS ? bytes : std::vector<char> {};
    }

    bool setState (const std::vector<char>& bytes) override
    {
        const auto* state = static_cast<const LV2_State_Interface*> (gDescriptor->extension_data (LV2_STATE__interface));
        std::vector<char> held = bytes;
        held.push_back (0); // So that an empty state still has somewhere to point.
        const auto status = state->restore (_instance, [] (LV2_State_Handle handle, uint32_t, size_t* size, uint32_t* type, uint32_t* flags) -> const void* {
            auto* self = static_cast<std::pair<std::vector<char>*, size_t>*> (handle);
            *size = self->second;
            *type = gUrids.get (LV2_ATOM__Chunk);
            *flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
            return self->first->data ();
        }, [&] { _retrieve = { &held, bytes.size () }; return &_retrieve; } (), 0, nullptr);
        return status == LV2_STATE_SUCCESS;
    }

    long allocationsInRun { 0 };
    long badMessageOutputs { 0 };
    long messagesOut { 0 };
    float latencyPort { -1.0f };

private:
    void forgeEvents (const std::vector<Note>& notes)
    {
        if (gTurtle.special.find ("events") == gTurtle.special.end ())
            return;

        const auto capacity = static_cast<uint32_t> (_eventBuffer.size ());
        lv2_atom_forge_set_buffer (&_forge, _eventBuffer.data (), capacity);
        LV2_Atom_Forge_Frame sequenceFrame;
        lv2_atom_forge_sequence_head (&_forge, &sequenceFrame, 0);

        if (_hasPendingTransport)
        {
            _hasPendingTransport = false;
            lv2_atom_forge_frame_time (&_forge, 0);
            LV2_Atom_Forge_Frame frame;
            lv2_atom_forge_object (&_forge, &frame, 0, gUrids.get (LV2_TIME__Position));
            lv2_atom_forge_key (&_forge, gUrids.get (LV2_TIME__speed));
            lv2_atom_forge_float (&_forge, _pendingTransport.isPlaying ? 1.0f : 0.0f);
            lv2_atom_forge_key (&_forge, gUrids.get (LV2_TIME__beatsPerMinute));
            lv2_atom_forge_float (&_forge, static_cast<float> (_pendingTransport.tempo));
            lv2_atom_forge_key (&_forge, gUrids.get (LV2_TIME__beat));
            lv2_atom_forge_double (&_forge, _pendingTransport.beat);
            lv2_atom_forge_key (&_forge, gUrids.get (LV2_TIME__beatsPerBar));
            lv2_atom_forge_float (&_forge, 4.0f);
            lv2_atom_forge_key (&_forge, gUrids.get (LV2_TIME__beatUnit));
            lv2_atom_forge_int (&_forge, 4);
            lv2_atom_forge_pop (&_forge, &frame);
        }

        const LV2_URID midiEvent = gUrids.get (LV2_MIDI__MidiEvent);
        for (const auto& note : notes)
        {
            const uint8_t message[3] = { note.status, note.pitch, note.velocity };
            lv2_atom_forge_frame_time (&_forge, static_cast<int64_t> (note.position));
            lv2_atom_forge_atom (&_forge, 3, midiEvent);
            lv2_atom_forge_write (&_forge, message, 3);
        }
        lv2_atom_forge_pop (&_forge, &sequenceFrame);
    }

    LV2_Handle _instance { nullptr };
    int32_t _maxBlock { 0 };
    std::pair<std::vector<char>*, size_t> _retrieve { nullptr, 0 };
    std::vector<float> _controls;
    std::vector<uint8_t> _eventBuffer;
    std::vector<uint8_t> _messageInput;
    std::vector<uint8_t> _messageOutput;
    LV2_Atom_Forge _forge {};
    Transport _pendingTransport {};
    bool _hasPendingTransport { false };
};

class Random
{
public:
    explicit Random (uint32_t seed) : _state (seed * 2654435761u + 0x9E3779B9u) {}

    uint32_t next ()
    {
        _state ^= _state << 13;
        _state ^= _state >> 17;
        _state ^= _state << 5;
        return _state;
    }

    double unit () { return next () / 4294967296.0; }

private:
    uint32_t _state;
};

struct Signal
{
    std::vector<float> left;
    std::vector<float> right;
};

// Tones and noise bursts with gaps of silence, different on each channel, or nothing for an instrument.
Signal makeSignal (uint32_t seed, size_t count, double sampleRate)
{
    Signal signal;
    if (!gHasAudioInput)
        return signal;

    signal.left.resize (count);
    signal.right.resize (count);
    Random random (seed);
    const double a = 110.0 + 400.0 * random.unit ();
    const double b = 700.0 + 3000.0 * random.unit ();
    const double c = 5000.0 + 9000.0 * random.unit ();
    for (size_t i = 0; i < count; ++i)
    {
        const double t = static_cast<double> (i) / sampleRate;
        const double phase = std::fmod (t, 1.0);
        const double envelope = phase < 0.7 ? std::min (1.0, phase * 50.0) * std::exp (-phase * 1.5) : 0.0;
        const double noise = phase > 0.3 && phase < 0.4 ? 0.3 * (2.0 * random.unit () - 1.0) : 0.0;
        const double tones = 0.35 * std::sin (2.0 * kPi * a * t) + 0.2 * std::sin (2.0 * kPi * b * t) + 0.08 * std::sin (2.0 * kPi * c * t);
        signal.left[i] = static_cast<float> (envelope * (tones + noise));
        signal.right[i] = static_cast<float> (envelope * (0.8 * tones + 0.2 * std::sin (2.0 * kPi * a * 1.5 * t) - noise));
    }
    return signal;
}

// Sorted by position, as an LV2 event port has to be.
std::vector<Note> makeNotes (uint32_t seed, size_t count, double sampleRate)
{
    std::vector<Note> notes;
    if (!gHasMidi)
        return notes;

    Random random (seed);
    size_t position = 100;
    const auto step = static_cast<size_t> (0.3 * sampleRate);
    int previous = -1;
    while (position < count)
    {
        const int pitch = 36 + static_cast<int> (random.unit () * 60.0);
        notes.push_back ({ position, static_cast<uint8_t> (0x90 | (random.next () % 16)), static_cast<uint8_t> (pitch), static_cast<uint8_t> (1 + random.next () % 127) });
        if (previous >= 0)
        {
            // Sometimes legato, by letting go of the last note after the new one starts.
            const size_t release = random.unit () < 0.5 ? position + 50 : position - 50;
            notes.push_back ({ release, 0x80, static_cast<uint8_t> (previous), 0 });
        }
        previous = pitch;
        position += step + random.next () % step;
    }
    notes.push_back ({ count > 1000 ? count - 1000 : 0, 0x80, static_cast<uint8_t> (previous), 0 });
    std::stable_sort (notes.begin (), notes.end (), [] (const Note& a, const Note& b) { return a.position < b.position; });
    return notes;
}

using BlockAction = std::function<void (Driver&, size_t position, int count)>;

std::vector<float> render (Driver& driver, size_t total, const std::vector<int>& blockSizes, const std::vector<Note>& notes,
                           const Signal* signal = nullptr, const BlockAction& atBlock = nullptr, bool isInPlace = false)
{
    // In place means one buffer for each channel's input and output, which LV2 allows, so only that side does it.
    std::vector<float> output (total * 2);
    std::vector<float> inLeft (kMaxBlock);
    std::vector<float> inRight (kMaxBlock);
    std::vector<float> left (kMaxBlock);
    std::vector<float> right (kMaxBlock);
    std::vector<Note> blockNotes;

    size_t position = 0;
    size_t blockIndex = 0;
    size_t nextNote = 0;
    while (position < total)
    {
        const int count = static_cast<int> (std::min<size_t> (static_cast<size_t> (blockSizes[blockIndex++ % blockSizes.size ()]), total - position));
        if (atBlock)
            atBlock (driver, position, count);

        blockNotes.clear ();
        while (nextNote < notes.size () && notes[nextNote].position < position + static_cast<size_t> (count))
        {
            Note note = notes[nextNote++];
            note.position -= position;
            blockNotes.push_back (note);
        }

        for (int i = 0; i < count; ++i)
        {
            const size_t at = position + static_cast<size_t> (i);
            inLeft[static_cast<size_t> (i)] = signal && at < signal->left.size () ? signal->left[at] : 0.0f;
            inRight[static_cast<size_t> (i)] = signal && at < signal->right.size () ? signal->right[at] : 0.0f;
        }

        std::fill (left.begin (), left.end (), kUnwritten);
        std::fill (right.begin (), right.end (), kUnwritten);
        if (isInPlace && dynamic_cast<Lv2Driver*> (&driver) != nullptr)
        {
            std::copy_n (inLeft.begin (), count, left.begin ());
            std::copy_n (inRight.begin (), count, right.begin ());
            driver.render (count, blockNotes, left.data (), right.data (), left.data (), right.data ());
        }
        else
        {
            driver.render (count, blockNotes, inLeft.data (), inRight.data (), left.data (), right.data ());
        }

        for (int i = 0; i < count; ++i)
        {
            output[(position + static_cast<size_t> (i)) * 2] = left[static_cast<size_t> (i)];
            output[(position + static_cast<size_t> (i)) * 2 + 1] = right[static_cast<size_t> (i)];
        }
        position += static_cast<size_t> (count);
    }
    return output;
}

struct Control
{
    Vst::ParamID id;
    std::string title;
    int stepCount;
    bool isAutomatable;
};

std::vector<Control> gControls;

float controlAt (Vst::IEditController* controller, Vst::ParamID id, double normalized)
{
    return static_cast<float> (controller->normalizedParamToPlain (id, normalized));
}

void setRandom (Driver& driver, Vst::IEditController* controller, uint32_t seed)
{
    Random random (seed * 7919u + 1u);
    for (const auto& control : gControls)
    {
        const double normalized = random.unit ();
        if (control.isAutomatable)
            driver.setControl (control.id, controlAt (controller, control.id, normalized));
    }
}

struct Scenario
{
    std::string name;
    double sampleRate;
    std::function<std::vector<float> (Driver&, Vst::IEditController*)> play;
    bool mustHaveSound { false };
};

constexpr size_t kSeconds3 = 144000;

std::vector<Scenario> makeScenarios ()
{
    std::vector<Scenario> scenarios;

    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        scenarios.push_back ({ "default-" + std::to_string (static_cast<int> (rate)), rate, [rate] (Driver& driver, Vst::IEditController*) {
            const auto total = static_cast<size_t> (3.0 * rate);
            const auto signal = makeSignal (1, total, rate);
            return render (driver, total, { 256 }, makeNotes (1, total, rate), &signal);
        }, true });
    }

    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        scenarios.push_back ({ "seeded-" + std::to_string (seed), 48000.0, [seed] (Driver& driver, Vst::IEditController* controller) {
            setRandom (driver, controller, seed);
            const auto signal = makeSignal (seed + 10, kSeconds3, 48000.0);
            return render (driver, kSeconds3, { 256 }, makeNotes (seed, kSeconds3, 48000.0), &signal);
        } });
    }

    const std::vector<std::vector<int>> patterns = { { 32 }, { 64 }, { 1024 }, { 4096 }, { 1 }, { 7, 233, 64, 1, 1000, 511 } };
    for (size_t i = 0; i < patterns.size (); ++i)
    {
        scenarios.push_back ({ "blocks-" + std::to_string (i), 48000.0, [pattern = patterns[i]] (Driver& driver, Vst::IEditController* controller) {
            setRandom (driver, controller, 3);
            const size_t total = pattern.size () == 1 && pattern[0] == 1 ? 24000 : kSeconds3;
            const auto signal = makeSignal (3, total, 48000.0);
            return render (driver, total, pattern, makeNotes (3, total, 48000.0), &signal);
        } });
    }

    for (const double end : { 0.0, 1.0 })
    {
        scenarios.push_back ({ "extreme-" + std::to_string (static_cast<int> (end)), 48000.0, [end] (Driver& driver, Vst::IEditController* controller) {
            // A VST3 host's true ends, which a float can't always hold: see controlToNormalized in the wrapper. Volume
            // stays up, because its bottom is silence, and two silent renders would prove nothing.
            for (const auto& control : gControls)
            {
                if (control.title == "Volume" || !control.isAutomatable)
                    continue;
                if (auto* vst3 = dynamic_cast<Vst3Driver*> (&driver))
                    vst3->setNormalized (control.id, end);
                else
                    driver.setControl (control.id, controlAt (controller, control.id, end));
            }
            const auto signal = makeSignal (4, kSeconds3, 48000.0);
            return render (driver, kSeconds3, { 256 }, makeNotes (4, kSeconds3, 48000.0), &signal);
        } });
    }

    for (uint32_t seed = 1; seed <= 3; ++seed)
    {
        scenarios.push_back ({ "moving-" + std::to_string (seed), 48000.0, [seed] (Driver& driver, Vst::IEditController* controller) {
            Random random (seed + 500);
            std::vector<double> rates;
            std::vector<double> phases;
            for (size_t i = 0; i < gControls.size (); ++i)
            {
                rates.push_back (0.1 + 3.0 * random.unit ());
                phases.push_back (random.unit ());
            }
            const BlockAction moving = [&] (Driver& target, size_t position, int count) {
                for (size_t i = 0; i < gControls.size (); ++i)
                {
                    if (!gControls[i].isAutomatable)
                        continue;
                    const auto valueAt = [&] (size_t at) {
                        return 0.5 + 0.5 * std::sin (2.0 * kPi * (rates[i] * static_cast<double> (at) / 48000.0 + phases[i]));
                    };
                    const float last = controlAt (controller, gControls[i].id, valueAt (position + static_cast<size_t> (count) - 1));
                    if (auto* vst3 = dynamic_cast<Vst3Driver*> (&target))
                        vst3->setRamp (gControls[i].id, valueAt (position), vst3->normalizedForControl (gControls[i].id, last));
                    else
                        target.setControl (gControls[i].id, last);
                }
            };
            const auto signal = makeSignal (seed + 20, kSeconds3, 48000.0);
            return render (driver, kSeconds3, { 128 }, makeNotes (seed, kSeconds3, 48000.0), &signal, moving);
        } });
    }

    if (gTurtle.supportsTransport)
    {
        // Stopped, playing, tempo changes and locates. A position is only sent when something jumps, so the rest of the
        // time this tests the wrapper's own counting against the host's.
        for (uint32_t seed = 1; seed <= 4; ++seed)
        {
            scenarios.push_back ({ "transport-" + std::to_string (seed), 48000.0, [seed] (Driver& driver, Vst::IEditController* controller) {
                setRandom (driver, controller, seed + 30);
                const double tempo = 60.0 + 40.0 * seed;
                const bool isPlaying = seed != 4;
                double beat = 0.0;
                const BlockAction change = [&] (Driver& target, size_t position, int) {
                    if (position == 0)
                        target.setTransport ({ tempo, beat, isPlaying });
                    else if (position == 48128)
                        target.setTransport ({ tempo * 1.5, 48128 * tempo / (60.0 * 48000.0), isPlaying });
                    else if (position == 96000)
                        target.setTransport ({ tempo * 1.5, 16.0, isPlaying });
                };
                const auto signal = makeSignal (seed + 30, kSeconds3, 48000.0);
                return render (driver, kSeconds3, { 256 }, makeNotes (seed, kSeconds3, 48000.0), &signal, change);
            } });
        }

        // Every synced setting, because a wrong tempo only shows when something follows it.
        scenarios.push_back ({ "transport-synced", 48000.0, [] (Driver& driver, Vst::IEditController* controller) {
            for (const auto& control : gControls)
            {
                if (control.title.find ("ync") != std::string::npos)
                    driver.setControl (control.id, controlAt (controller, control.id, 1.0));
                else if (control.title == "LFO amount" || control.title == "Feedback")
                    driver.setControl (control.id, controlAt (controller, control.id, 0.8));
            }
            const BlockAction change = [] (Driver& target, size_t position, int) {
                if (position == 0)
                    target.setTransport ({ 137.5, 3.25, true });
            };
            const auto signal = makeSignal (31, kSeconds3, 48000.0);
            return render (driver, kSeconds3, { 256 }, makeNotes (31, kSeconds3, 48000.0), &signal, change);
        } });
    }

    if (gHasAudioInput)
    {
        scenarios.push_back ({ "in-place", 48000.0, [] (Driver& driver, Vst::IEditController* controller) {
            setRandom (driver, controller, 8);
            const auto signal = makeSignal (8, kSeconds3, 48000.0);
            return render (driver, kSeconds3, { 256 }, {}, &signal, nullptr, true);
        }, true });
    }

    // Long gaps, so that a voice goes to sleep, and is woken at the very start of a block.
    scenarios.push_back ({ "sleep", 48000.0, [] (Driver& driver, Vst::IEditController*) {
        std::vector<Note> notes;
        Signal signal;
        if (gHasMidi)
        {
            for (size_t start = 0; start < 480000; start += 96000)
            {
                notes.push_back ({ start + 5120, 0x90, 60, 100 });
                notes.push_back ({ start + 15000, 0x80, 60, 0 });
            }
        }
        if (gHasAudioInput)
        {
            const auto tones = makeSignal (40, 48000, 48000.0);
            signal.left.assign (96000, 0.0f);
            signal.right.assign (96000, 0.0f);
            signal.left.insert (signal.left.end (), tones.left.begin (), tones.left.end ());
            signal.right.insert (signal.right.end (), tones.right.begin (), tones.right.end ());
        }
        return render (driver, 480000, { 256 }, notes, &signal);
    }, true });

    if (gHasAudioInput)
    {
        scenarios.push_back ({ "broken-input", 48000.0, [] (Driver& driver, Vst::IEditController*) {
            auto signal = makeSignal (41, 96000, 48000.0);
            for (size_t i = 24000; i < 24100; ++i)
            {
                signal.left[i] = std::numeric_limits<float>::quiet_NaN ();
                signal.right[i] = std::numeric_limits<float>::infinity ();
            }
            return render (driver, 96000, { 256 }, {}, &signal);
        } });
    }

    if (gHasMidi)
    {
        // Things a host can send, that a VST3 host wouldn't pass on or that have special meanings in MIDI.
        scenarios.push_back ({ "note-edges", 48000.0, [] (Driver& driver, Vst::IEditController*) {
            const std::vector<Note> notes = {
                { 0, 0x90, 60, 100 },    // At a block's first sample.
                { 255, 0x90, 64, 100 },  // At its last.
                { 256, 0x80, 60, 0 },
                { 300, 0xB0, 64, 127 },  // Sustain and the like are ignored.
                { 301, 0x80, 99, 0 },    // A note that was never played.
                { 400, 0x90, 64, 0 },    // A note-on with no velocity is a note-off.
                { 2000, 0x9F, 127, 1 },  // Another channel, and the top of the range.
                { 2000, 0x8F, 127, 1 },  // Both in the same sample.
                { 2100, 0x90, 0, 127 },
                { 50000, 0x80, 0, 64 },
            };
            return render (driver, 96000, { 256 }, notes);
        }, true });

        // A control changing in the same block as a note, which matters while a voice is asleep.
        scenarios.push_back ({ "change-with-note", 48000.0, [] (Driver& driver, Vst::IEditController* controller) {
            const BlockAction change = [controller] (Driver& target, size_t position, int) {
                if (position == 25600)
                    target.setControl (gControls[6].id, controlAt (controller, gControls[6].id, 0.2));
            };
            return render (driver, 96000, { 256 }, { { 25610, 0x90, 48, 100 }, { 60000, 0x80, 48, 0 } }, nullptr, change);
        }, true });
    }

    // Stopping and starting in place, which resets the DSP.
    scenarios.push_back ({ "reactivate", 48000.0, [] (Driver& driver, Vst::IEditController* controller) {
        setRandom (driver, controller, 50);
        const auto notes = makeNotes (50, 96000, 48000.0);
        const auto signal = makeSignal (50, 96000, 48000.0);
        auto first = render (driver, 96000, { 256 }, notes, &signal);
        driver.reactivate ();
        const auto second = render (driver, 96000, { 256 }, notes, &signal);
        first.insert (first.end (), second.begin (), second.end ());
        return first;
    } });

    return scenarios;
}

std::string describeDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size () != b.size ())
        return "different lengths";
    size_t first = a.size ();
    size_t count = 0;
    double peak = 0.0;
    for (size_t i = 0; i < a.size (); ++i)
    {
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
        {
            first = std::min (first, i);
            ++count;
            if (std::isfinite (a[i]) && std::isfinite (b[i]))
                peak = std::max (peak, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));
        }
    }
    if (count == 0)
        return "";
    char text[128];
    std::snprintf (text, sizeof (text), "%zu of %zu samples differ, the first at %zu, peak %g", count, a.size (), first, peak);
    return text;
}

bool hasSound (const std::vector<float>& samples)
{
    return std::any_of (samples.begin (), samples.end (), [] (float sample) { return sample != 0.0f; });
}

int gFailures = 0;

void check (bool isOk, const std::string& what)
{
    std::printf ("%s  %s\n", isOk ? "ok  " : "FAIL", what.c_str ());
    std::fflush (stdout);
    if (!isOk)
        ++gFailures;
}

std::string narrow (const Vst::TChar* text)
{
    std::string result;
    for (; *text != 0; ++text)
        result += static_cast<char> (*text);
    return result;
}

bool readTurtle (const fs::path& file)
{
    std::ifstream stream (file);
    if (!stream)
        return false;
    std::stringstream buffer;
    buffer << stream.rdbuf ();
    const std::string text = buffer.str ();

    gTurtle.supportsTransport = text.find ("atom:supports time:Position") != std::string::npos;
    for (size_t at = text.find ("lv2:designation lv2:control"); at != std::string::npos; at = text.find ("lv2:designation lv2:control", at + 1))
        ++gTurtle.controlDesignations;
    gTurtle.supportsMidi = text.find ("atom:supports midi:MidiEvent") != std::string::npos;

    const auto capture = [] (const std::string& block, const char* pattern, std::string& value) {
        std::smatch match;
        if (!std::regex_search (block, match, std::regex (pattern)))
            return false;
        value = match[1];
        return true;
    };

    size_t start = text.find ("lv2:port [");
    while (start != std::string::npos)
    {
        const size_t next = text.find ("lv2:port [", start + 1);
        const std::string block = text.substr (start, next == std::string::npos ? std::string::npos : next - start);
        start = next;

        Port port;
        std::string value;
        if (!capture (block, "lv2:index ([0-9]+)", value))
            return false;
        port.index = static_cast<uint32_t> (std::stoul (value));
        if (!capture (block, "lv2:symbol \"([^\"]+)\"", port.symbol) || !capture (block, "lv2:name \"([^\"]+)\"", port.name))
            return false;

        const bool isControlInput = block.find ("lv2:InputPort , lv2:ControlPort") != std::string::npos;
        if (!isControlInput)
        {
            gTurtle.special[port.symbol] = port.index;
            continue;
        }

        if (!capture (block, "lv2:minimum ([^ ]+)", value))
            return false;
        port.minimum = std::stod (value);
        if (!capture (block, "lv2:maximum ([^ ]+)", value))
            return false;
        port.maximum = std::stod (value);
        if (!capture (block, "lv2:default ([^ ]+)", value))
            return false;
        port.defaultValue = std::stod (value);
        gTurtle.controls.push_back (port);
    }
    std::sort (gTurtle.controls.begin (), gTurtle.controls.end (), [] (const Port& a, const Port& b) { return a.index < b.index; });
    return true;
}

bool isClose (double a, double b)
{
    return std::abs (a - b) <= 1e-9 * std::max ({ 1.0, std::abs (a), std::abs (b) });
}

// That the Turtle and the VST3 describe the same plug-in, which only a host would otherwise find out.
// Pango's font thread outlives a panel, so a host that unloads the LV2 while it's busy would crash. The library has to
// say it can't be unloaded, with -z nodelete, and this reads that from the loaded library rather than unloading it.
bool isNoDelete (void* module)
{
    link_map* map = nullptr;
    if (dlinfo (module, RTLD_DI_LINKMAP, &map) != 0 || !map)
        return false;
    for (const ElfW (Dyn)* entry = map->l_ld; entry->d_tag != DT_NULL; ++entry)
        if (entry->d_tag == DT_FLAGS_1)
            return (entry->d_un.d_val & DF_1_NODELETE) != 0;
    return false;
}

void checkTurtle ()
{
    Vst3Driver reference (48000.0);
    check (gTurtle.special.count ("out_left") == 1 && gTurtle.special.count ("out_right") == 1, "the Turtle has stereo outputs");
    check (reference.hasInput () == (gTurtle.special.count ("in_left") == 1 && gTurtle.special.count ("in_right") == 1), "the Turtle agrees about audio input");
    check (reference.hasEvents () == gTurtle.supportsMidi, "the Turtle agrees about MIDI");

    // REAPER sends a UI's messages to the port marked as the control port, so with message ports, that has to be ui_in
    // alone, and the transport still finds the events port by what it supports.
    const bool hasEventPort = gTurtle.special.count ("events") == 1 || gTurtle.special.count ("ui_in") == 1;
    check (gTurtle.controlDesignations == (hasEventPort ? 1 : 0), "the Turtle marks one atom input as the control port: " + std::to_string (gTurtle.controlDesignations));
    check (gTurtle.controls.size () == gControls.size (), "the Turtle has a control for each parameter: " + std::to_string (gTurtle.controls.size ()));
    if (gTurtle.controls.size () != gControls.size ())
        return;

    uint32_t expectedIndex = gTurtle.controls.front ().index;
    bool isInOrder = true;
    bool isSame = true;
    for (size_t i = 0; i < gControls.size (); ++i)
    {
        const auto& port = gTurtle.controls[i];
        const auto& control = gControls[i];
        isInOrder = isInOrder && port.index == expectedIndex++ && control.id == i;

        Vst::ParameterInfo info {};
        reference.controller->getParameterInfo (static_cast<int32> (i), info);
        const double minimum = reference.controller->normalizedParamToPlain (control.id, 0.0);
        const double maximum = reference.controller->normalizedParamToPlain (control.id, 1.0);
        const double defaultValue = reference.controller->normalizedParamToPlain (control.id, info.defaultNormalizedValue);
        if (port.name != control.title || !isClose (port.minimum, minimum) || !isClose (port.maximum, maximum) || !isClose (port.defaultValue, defaultValue))
        {
            std::printf ("     %s: Turtle %s %g %g %g, VST3 %s %g %g %g\n", port.symbol.c_str (), port.name.c_str (), port.minimum, port.maximum, port.defaultValue,
                         control.title.c_str (), minimum, maximum, defaultValue);
            isSame = false;
        }
    }
    check (isInOrder, "controls follow the parameters, in order");
    check (isSame, "names, ranges and defaults match the VST3's");
}

void checkLatency ()
{
    const auto latency = gTurtle.special.find ("latency");
    for (const double rate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        Vst3Driver vst3 (rate);
        const uint32 expected = vst3.processor->getLatencySamples ();
        if (latency == gTurtle.special.end ())
        {
            check (expected == 0, "no latency port, and the VST3 has none at " + std::to_string (static_cast<int> (rate)));
            continue;
        }
        Lv2Driver lv2 (rate);
        std::vector<float> buffers[4];
        for (auto& buffer : buffers)
            buffer.resize (kMaxBlock);
        lv2.render (64, {}, buffers[0].data (), buffers[1].data (), buffers[2].data (), buffers[3].data ());
        check (lv2.latencyPort == static_cast<float> (expected), "latency at " + std::to_string (static_cast<int> (rate)) + " is " + std::to_string (expected));
    }
}

std::vector<char> craftState (int32_t version, int32_t count, const std::vector<std::pair<int32_t, double>>& pairs)
{
    std::vector<char> bytes;
    const auto append = [&bytes] (const void* data, size_t size) {
        const auto* start = static_cast<const char*> (data);
        bytes.insert (bytes.end (), start, start + size);
    };
    append (&version, sizeof (version));
    append (&count, sizeof (count));
    for (const auto& pair : pairs)
    {
        append (&pair.first, sizeof (pair.first));
        append (&pair.second, sizeof (pair.second));
    }
    return bytes;
}

// The same bytes as the VST3's, in both directions, so that a host's saved LV2 state can be read by the same code.
void checkState ()
{
    const auto notes = makeNotes (60, 48000, 48000.0);
    const auto signal = makeSignal (60, 48000, 48000.0);
    const auto play = [&] (Driver& driver) { return render (driver, 48000, { 256 }, notes, &signal); };

    {
        Vst3Driver vst3 (48000.0);
        Lv2Driver lv2 (48000.0);
        check (vst3.getState () == lv2.getState (), "state: a new instance saves the same bytes");

        setRandom (vst3, vst3.controller, 61);
        setRandom (lv2, vst3.controller, 61);
        play (vst3);
        play (lv2);
        check (vst3.getState () == lv2.getState (), "state: after settings and playing, it saves the same bytes");
    }

    // Exact values, which a port can't hold.
    std::vector<std::pair<int32_t, double>> exact;
    {
        Random random (62);
        for (const auto& control : gControls)
            exact.push_back ({ static_cast<int32_t> (control.id), random.unit () });
    }
    int32_t version = 1;
    {
        Vst3Driver vst3 (48000.0);
        const auto bytes = vst3.getState ();
        std::memcpy (&version, bytes.data (), sizeof (version));
    }
    const auto count = static_cast<int32_t> (exact.size ());
    const auto saved = craftState (version, count, exact);

    const auto compareAfter = [&] (const std::string& name, const std::function<void (Lv2Driver&, Vst::IEditController*)>& afterRestore) {
        Vst3Driver vst3 (48000.0);
        Lv2Driver lv2 (48000.0);
        vst3.setState (saved);
        const bool isRestored = lv2.setState (saved);
        check (isRestored, "state: " + name + ": restores");
        afterRestore (lv2, vst3.controller);
        const auto expected = play (vst3);
        const auto actual = play (lv2);
        const auto difference = describeDifference (expected, actual);
        check (difference.empty (), "state: " + name + (difference.empty () ? "" : ": " + difference));
        check (vst3.getState () == lv2.getState (), "state: " + name + ": saves the same bytes again");
    };

    // Hosts restore ports and state in no fixed order, so all of these have to leave the state's exact values alone.
    compareAfter ("ports not touched", [] (Lv2Driver&, Vst::IEditController*) {});
    compareAfter ("ports written with the state's rounded values", [&] (Lv2Driver& lv2, Vst::IEditController* controller) {
        for (size_t i = 0; i < exact.size (); ++i)
            lv2.setControl (gControls[i].id, controlAt (controller, gControls[i].id, exact[i].second));
    });

    // After restoring, moving a port has to work as usual.
    {
        Vst3Driver vst3 (48000.0);
        Lv2Driver lv2 (48000.0);
        vst3.setState (saved);
        lv2.setState (saved);
        const float moved = controlAt (vst3.controller, gControls[0].id, 0.123);
        vst3.setControl (gControls[0].id, moved);
        lv2.setControl (gControls[0].id, moved);
        const auto difference = describeDifference (play (vst3), play (lv2));
        check (difference.empty (), "state: a port moved after restoring" + (difference.empty () ? std::string () : ": " + difference));
    }

    // Damaged state has to do just what it does in the VST3.
    auto oneNan = exact;
    oneNan[0].second = std::numeric_limits<double>::quiet_NaN ();
    auto outOfRange = exact;
    outOfRange[0].second = 2.0;
    auto truncated = saved;
    truncated.resize (truncated.size () - 5);
    const std::vector<std::pair<std::string, std::vector<char>>> damaged = {
        { "future version", craftState (99, count, exact) },
        { "a bad value", craftState (version, count, oneNan) },
        { "out of range", craftState (version, count, outOfRange) },
        { "count too big", craftState (version, count + 5, exact) },
        { "count negative", craftState (version, -1, exact) },
        { "truncated", truncated },
        { "only a version", std::vector<char> (4, 1) },
        { "empty", {} },
    };
    for (const auto& [name, bytes] : damaged)
    {
        Vst3Driver vst3 (48000.0);
        Lv2Driver lv2 (48000.0);
        const bool expected = vst3.setState (bytes);
        const bool actual = lv2.setState (bytes);
        check (expected == actual, "state: damaged, " + name + ": same answer, " + (expected ? "accepted" : "refused"));
        const auto difference = describeDifference (play (vst3), play (lv2));
        check (difference.empty (), "state: damaged, " + name + (difference.empty () ? std::string () : ": " + difference));
        check (vst3.getState () == lv2.getState (), "state: damaged, " + name + ": saves the same bytes");
    }
}

// That this test can fail.
void checkNegatives ()
{
    const auto notes = makeNotes (7, kSeconds3, 48000.0);
    const auto signal = makeSignal (7, kSeconds3, 48000.0);

    if (gHasMidi)
    {
        auto lateNotes = notes;
        lateNotes.front ().position += 1;
        Lv2Driver a (48000.0);
        Lv2Driver b (48000.0);
        check (!describeDifference (render (a, kSeconds3, { 256 }, notes), render (b, kSeconds3, { 256 }, lateNotes)).empty (), "a note a sample late is noticed");
    }
    if (gHasAudioInput)
    {
        auto nudged = signal;
        nudged.left[100] += 0.25f;
        Lv2Driver a (48000.0);
        Lv2Driver b (48000.0);
        check (!describeDifference (render (a, kSeconds3, { 256 }, {}, &signal), render (b, kSeconds3, { 256 }, {}, &nudged)).empty (), "a changed input sample is noticed");
    }

    // From the middle of each range, because a default can be somewhere that a step does nothing, such as the top of
    // a range or a mix of zero. Some controls do nothing whatever their value, so one is enough.
    bool isNoticed = false;
    Vst3Driver reference (48000.0);
    for (const auto& control : gControls)
    {
        if (control.stepCount != 0 || isNoticed)
            continue;
        const float middle = controlAt (reference.controller, control.id, 0.5);
        Lv2Driver a (48000.0);
        Lv2Driver b (48000.0);
        a.setControl (control.id, middle);
        b.setControl (control.id, std::nextafter (middle, std::numeric_limits<float>::max ()));
        isNoticed = !describeDifference (render (a, kSeconds3, { 256 }, notes, &signal), render (b, kSeconds3, { 256 }, notes, &signal)).empty ();
    }
    check (isNoticed, "a control one float step away is noticed");

    // Two at once, because they share a module.
    {
        Lv2Driver one (48000.0);
        Lv2Driver two (48000.0);
        Lv2Driver alone (48000.0);
        const BlockAction interleave = [&two] (Driver&, size_t position, int count) {
            std::vector<float> left (kMaxBlock);
            std::vector<float> right (kMaxBlock);
            std::vector<float> inLeft (kMaxBlock, position < 2000 ? 0.5f : 0.0f);
            std::vector<float> inRight (kMaxBlock, 0.0f);
            if (position == 0 && gHasMidi)
                two.render (count, { { 100, 0x90, 72, 90 } }, inLeft.data (), inRight.data (), left.data (), right.data ());
            else
                two.render (count, {}, inLeft.data (), inRight.data (), left.data (), right.data ());
        };
        const auto together = render (one, kSeconds3, { 256 }, notes, &signal, interleave);
        const auto separate = render (alone, kSeconds3, { 256 }, notes, &signal);
        check (describeDifference (together, separate).empty (), "two instances don't disturb each other");
    }
}

// How far a VST3 given unrounded values is from one given the nearest float, which an LV2 host can't avoid.
void reportFloatSteps ()
{
    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        const auto notes = makeNotes (seed, kSeconds3, 48000.0);
        const auto signal = makeSignal (seed + 10, kSeconds3, 48000.0);
        Vst3Driver rounded (48000.0);
        Vst3Driver exact (48000.0);
        Random random (seed * 7919u + 1u);
        for (const auto& control : gControls)
        {
            const double normalized = random.unit ();
            if (!control.isAutomatable)
                continue;
            exact.setNormalized (control.id, normalized);
            rounded.setControl (control.id, controlAt (rounded.controller, control.id, normalized));
        }
        const auto a = render (exact, kSeconds3, { 256 }, notes, &signal);
        const auto b = render (rounded, kSeconds3, { 256 }, notes, &signal);

        double peakDifference = 0.0;
        double peak = 0.0;
        for (size_t i = 0; i < a.size (); ++i)
        {
            peakDifference = std::max (peakDifference, std::abs (static_cast<double> (a[i]) - static_cast<double> (b[i])));
            peak = std::max (peak, std::abs (static_cast<double> (a[i])));
        }
        std::printf ("float steps, seed %2u: peak %.3f, difference %.1f dB below full scale\n", seed, peak,
                     peakDifference > 0.0 ? -20.0 * std::log10 (peakDifference) : 999.0);
    }
}

} // namespace

// A plug-in's own messages arrive in real time, from a thread of the instance's, so the scenarios are too quick to see
// any. This runs in real time for long enough.
void checkMessagePorts ()
{
    if (gTurtle.special.find ("ui_out") == gTurtle.special.end ())
        return;

    Lv2Driver lv2 (48000.0);
    std::vector<float> silence (256, 0.0f);
    std::vector<float> left (256);
    std::vector<float> right (256);
    for (int i = 0; i < 100; ++i)
    {
        lv2.render (256, {}, silence.data (), silence.data (), left.data (), right.data ());
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    check (lv2.messagesOut > 0, "messages come out of the message port: " + std::to_string (lv2.messagesOut));
    check (lv2.badMessageOutputs == 0, "  as a sequence of whole message objects after every run ()");
    check (lv2.allocationsInRun == 0, "  with no allocations in run (): " + std::to_string (lv2.allocationsInRun));
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf ("Usage: <program> <VST3 .so> <LV2 bundle folder> [scenario name contains]\n");
        return 2;
    }
    const fs::path bundle = fs::absolute (argv[2]);
    // Not from the bundle's name, because the shallow path test puts a copy at a temporary one.
    std::string stem;
    for (const auto& entry : fs::directory_iterator (bundle))
    {
        if (entry.path ().extension () == ".so")
            stem = entry.path ().stem ().string ();
    }
    const std::string filter = argc > 3 ? argv[3] : "";

    void* vst3Module = dlopen (argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!vst3Module)
    {
        std::printf ("Could not load the VST3: %s\n", dlerror ());
        return 2;
    }
    void* lv2Module = dlopen ((bundle / (stem + ".so")).c_str (), RTLD_LAZY | RTLD_LOCAL);
    if (!lv2Module)
    {
        std::printf ("Could not load the LV2: %s\n", dlerror ());
        return 2;
    }
    if (!readTurtle (bundle / (stem + ".ttl")))
    {
        std::printf ("Could not read the Turtle\n");
        return 2;
    }

    auto moduleEntry = reinterpret_cast<bool (*) (void*)> (dlsym (vst3Module, "ModuleEntry"));
    auto moduleExit = reinterpret_cast<bool (*) ()> (dlsym (vst3Module, "ModuleExit"));
    auto getFactory = reinterpret_cast<GetFactoryProc> (dlsym (vst3Module, "GetPluginFactory"));
    auto getDescriptor = reinterpret_cast<const LV2_Descriptor* (*) (uint32_t)> (dlsym (lv2Module, "lv2_descriptor"));
    if (!moduleEntry || !moduleExit || !getFactory || !getDescriptor || !moduleEntry (vst3Module))
    {
        std::printf ("Missing entry points\n");
        return 2;
    }

    gFactory = getFactory ();
    for (int32 i = 0; i < gFactory->countClasses (); ++i)
    {
        PClassInfo info;
        gFactory->getClassInfo (i, &info);
        if (std::strcmp (info.category, kVstAudioEffectClass) == 0)
            std::memcpy (gComponentId, info.cid, sizeof (TUID));
        else if (std::strcmp (info.category, kVstComponentControllerClass) == 0)
            std::memcpy (gControllerId, info.cid, sizeof (TUID));
    }

    gDescriptor = getDescriptor (0);
    check (gDescriptor != nullptr && getDescriptor (1) == nullptr, "one descriptor");
    if (!gDescriptor)
        return 1;
    std::printf ("uri %s\n", gDescriptor->URI);

    {
        Vst3Driver reference (48000.0);
        gHasAudioInput = reference.hasInput ();
        gHasMidi = reference.hasEvents ();
        for (int32 i = 0; i < reference.controller->getParameterCount (); ++i)
        {
            Vst::ParameterInfo info {};
            reference.controller->getParameterInfo (i, info);
            gControls.push_back ({ info.id, narrow (info.title), info.stepCount, (info.flags & Vst::ParameterInfo::kCanAutomate) != 0 });
        }
    }
    checkTurtle ();

    // Without what the Turtle requires, a plug-in has to refuse rather than crash.
    {
        const LV2_Feature* const none[] = { nullptr };
        check (gDescriptor->instantiate (gDescriptor, 48000.0, "", none) == nullptr, "refuses a host without the required features");
    }

    // The counter's own check: making an instance allocates, so it has to see that from inside the plug-in.
    {
        const long before = gAllocations;
        Lv2Driver driver (48000.0);
        check (driver.isValid (), "instantiates");
        check (gAllocations > before, "the allocation counter sees the plug-in's allocations");
    }

    int withSound = 0;
    int compared = 0;
    for (const auto& scenario : makeScenarios ())
    {
        if (scenario.name.find (filter) == std::string::npos)
            continue;

        Vst3Driver vst3 (scenario.sampleRate);
        Lv2Driver lv2 (scenario.sampleRate);
        const auto expected = scenario.play (vst3, vst3.controller);
        const auto actual = scenario.play (lv2, vst3.controller);

        const auto difference = describeDifference (expected, actual);
        check (difference.empty (), scenario.name + (difference.empty () ? "" : ": " + difference));
        const bool sounded = hasSound (expected);
        ++compared;
        withSound += sounded ? 1 : 0;
        if (scenario.mustHaveSound)
            check (sounded, "  has sound");
        check (std::find (actual.begin (), actual.end (), kUnwritten) == actual.end (), "  writes every sample");
        check (lv2.allocationsInRun == 0, "  no allocations in run (): " + std::to_string (lv2.allocationsInRun));
        check (lv2.badMessageOutputs == 0, "  the message port always holds a sequence after run ()");
    }

    if (filter.empty ())
    {
        check (withSound * 2 > compared, std::to_string (withSound) + " of " + std::to_string (compared) + " scenarios have sound");
        checkLatency ();
        checkState ();
        checkNegatives ();
        checkMessagePorts ();
        reportFloatSteps ();
        check (isNoDelete (lv2Module), "the LV2 can't be unloaded (-z nodelete)");
    }

    gFactory->release ();
    moduleExit ();
    std::printf ("%d failure(s)\n", gFailures);
    std::fflush (stdout);
    // Not unloading, because the modules' static objects outlive this point in a test program, and unloading is covered
    // by the VST3's own tests.
    return gFailures == 0 ? 0 : 1;
}
