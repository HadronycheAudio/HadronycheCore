// Usage: NullTest <path to a plug-in's DLL or .so> <output folder>
//
// Dumps everything a host can observe about a built plug-in into a folder: its classes, buses, parameters
// and their text, saved state, how it reads old and damaged state, and rendered audio across many settings.
// Comparing the folders of two builds with compare.py shows whether a refactoring changed anything.
//
// It only uses the VST3 interfaces, so it doesn't depend on any plug-in's code, and it's the same binary on
// both sides of a comparison. If this file changes, the baseline has to be dumped again.

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
namespace fs = std::filesystem;

DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (Vst::IHostApplication)

namespace {

constexpr double kPi = 3.14159265358979;
constexpr int kMaxBlock = 4096;

#define NULLTEST_NO_REFCOUNT                                                                  \
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }  \
    uint32 PLUGIN_API addRef () override { return 1; }                                        \
    uint32 PLUGIN_API release () override { return 1; }

class Attributes : public Vst::IAttributeList
{
public:
    NULLTEST_NO_REFCOUNT

    tresult PLUGIN_API setInt (AttrID id, int64 value) override
    {
        _ints[id] = value;
        return kResultOk;
    }

    tresult PLUGIN_API getInt (AttrID id, int64& value) override
    {
        const auto it = _ints.find (id);
        if (it == _ints.end ())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }

    tresult PLUGIN_API setFloat (AttrID, double) override { return kResultFalse; }
    tresult PLUGIN_API getFloat (AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString (AttrID, const Vst::TChar*) override { return kResultFalse; }
    tresult PLUGIN_API getString (AttrID, Vst::TChar*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary (AttrID, const void*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API getBinary (AttrID, const void*&, uint32&) override { return kResultFalse; }

private:
    std::map<std::string, int64> _ints;
};

class Message : public Vst::IMessage
{
public:
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef () override { return static_cast<uint32> (++_references); }

    uint32 PLUGIN_API release () override
    {
        const int left = --_references;
        if (left == 0)
            delete this;
        return static_cast<uint32> (left);
    }

    FIDString PLUGIN_API getMessageID () override { return _id.c_str (); }
    void PLUGIN_API setMessageID (FIDString id) override { _id = id; }
    Vst::IAttributeList* PLUGIN_API getAttributes () override { return &_attributes; }

private:
    int _references { 1 };
    std::string _id;
    Attributes _attributes;
};

class Host : public Vst::IHostApplication
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Vst::IHostApplication::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<Vst::IHostApplication*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        name[0] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (TUID classId, TUID, void** object) override
    {
        if (FUnknownPrivate::iidEqual (classId, Vst::IMessage::iid))
        {
            *object = new Message;
            return kResultOk;
        }
        *object = nullptr;
        return kResultFalse;
    }
};

class ValueQueue : public Vst::IParamValueQueue
{
public:
    NULLTEST_NO_REFCOUNT

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
    NULLTEST_NO_REFCOUNT

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
    NULLTEST_NO_REFCOUNT

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

Host gHost;
IPluginFactory* gFactory = nullptr;
TUID gComponentId;
TUID gControllerId;
fs::path gOutput;

// A fixed sequence, so that every run uses the same settings and signals.
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

std::string narrow (const Vst::TChar* text)
{
    std::string result;
    for (; *text != 0; ++text)
    {
        if (*text >= 32 && *text < 127)
        {
            result += static_cast<char> (*text);
        }
        else
        {
            char escaped[16];
            std::snprintf (escaped, sizeof (escaped), "\\u%04x", static_cast<unsigned> (*text));
            result += escaped;
        }
    }
    return result;
}

std::string exact (double value)
{
    char text[64];
    std::snprintf (text, sizeof (text), "%a", value);
    return text;
}

std::string hexId (const TUID id)
{
    std::string result;
    for (int i = 0; i < 16; ++i)
    {
        char text[4];
        std::snprintf (text, sizeof (text), "%02X", static_cast<unsigned char> (id[i]));
        result += text;
    }
    return result;
}

void writeFile (const std::string& name, const void* data, size_t size)
{
    std::ofstream file (gOutput / name, std::ios::binary);
    file.write (static_cast<const char*> (data), static_cast<std::streamsize> (size));
}

void writeText (const std::string& name, const std::string& text)
{
    writeFile (name, text.data (), text.size ());
}

struct Output
{
    std::vector<float> interleaved;
    std::string flags; // The silence flags of each block.
};

struct Input
{
    std::vector<float> left;
    std::vector<float> right;
};

struct TimedEvent
{
    size_t position;
    Vst::Event event;
};

class Plug
{
public:
    explicit Plug (double sampleRate = 48000.0, bool activate = true) : _sampleRate (sampleRate)
    {
        gFactory->createInstance (gComponentId, Vst::IComponent::iid, reinterpret_cast<void**> (&component));
        component->initialize (&gHost);
        component->queryInterface (Vst::IAudioProcessor::iid, reinterpret_cast<void**> (&processor));

        gFactory->createInstance (gControllerId, Vst::IEditController::iid, reinterpret_cast<void**> (&controller));
        controller->initialize (&gHost);

        component->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_componentConnection));
        controller->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_controllerConnection));
        _componentConnection->connect (_controllerConnection);
        _controllerConnection->connect (_componentConnection);

        hasAudioInput = component->getBusCount (Vst::kAudio, Vst::kInput) > 0;
        hasEventInput = component->getBusCount (Vst::kEvent, Vst::kInput) > 0;

        const int32 count = controller->getParameterCount ();
        for (int32 i = 0; i < count; ++i)
        {
            Vst::ParameterInfo info {};
            controller->getParameterInfo (i, info);
            params.push_back (info);
        }

        Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, kMaxBlock, sampleRate };
        processor->setupProcessing (setup);
        if (hasAudioInput)
            component->activateBus (Vst::kAudio, Vst::kInput, 0, true);
        component->activateBus (Vst::kAudio, Vst::kOutput, 0, true);
        if (hasEventInput)
            component->activateBus (Vst::kEvent, Vst::kInput, 0, true);

        _isActive = activate;
        if (_isActive)
        {
            component->setActive (true);
            processor->setProcessing (true);
        }
    }

    ~Plug ()
    {
        if (_isActive)
        {
            processor->setProcessing (false);
            component->setActive (false);
        }
        _componentConnection->disconnect (_controllerConnection);
        _controllerConnection->disconnect (_componentConnection);
        _componentConnection->release ();
        _controllerConnection->release ();
        controller->terminate ();
        controller->release ();
        processor->release ();
        component->terminate ();
        component->release ();
    }

    Plug (const Plug&) = delete;
    Plug& operator= (const Plug&) = delete;

    // Takes effect in the next block, and reaches both sides, as a host would forward it.
    void set (Vst::ParamID id, double normalized)
    {
        controller->setParamNormalized (id, normalized);
        ValueQueue queue (id);
        queue.add (0, normalized);
        _changes.queues.push_back (queue);
    }

    bool setByTitle (const char* title, double plain)
    {
        for (const auto& info : params)
        {
            if (narrow (info.title) == title)
            {
                set (info.id, controller->plainParamToNormalized (info.id, plain));
                return true;
            }
        }
        return false;
    }

    bool hasParam (const char* title) const
    {
        return std::any_of (params.begin (), params.end (), [title] (const Vst::ParameterInfo& info) { return narrow (info.title) == title; });
    }

    void setAllAutomatable (const std::function<double (const Vst::ParameterInfo&)>& valueFor)
    {
        for (const auto& info : params)
        {
            if ((info.flags & Vst::ParameterInfo::kCanAutomate) != 0)
                set (info.id, valueFor (info));
        }
    }

    void sendCommand (const char* messageId, const char* attribute, int64 value)
    {
        Message message;
        message.addRef ();
        message.setMessageID (messageId);
        message.getAttributes ()->setInt (attribute, value);
        _componentConnection->notify (&message);
        message.release ();
    }

    // blockAt is called before each block, to change parameters as it goes. Silence is used when the input runs out.
    Output render (size_t total, const Input* input, const std::vector<int>& blockSizes,
                   const std::vector<TimedEvent>& events = {},
                   const std::function<void (size_t position, int count)>& blockAt = nullptr)
    {
        Output result;
        result.interleaved.resize (total * 2);

        std::vector<float> inLeft (kMaxBlock);
        std::vector<float> inRight (kMaxBlock);
        std::vector<float> outLeft (kMaxBlock);
        std::vector<float> outRight (kMaxBlock);

        size_t position = 0;
        size_t blockIndex = 0;
        while (position < total)
        {
            const int wanted = blockSizes[blockIndex++ % blockSizes.size ()];
            const int count = static_cast<int> (std::min<size_t> (static_cast<size_t> (wanted), total - position));

            if (blockAt)
                blockAt (position, count);

            for (int i = 0; i < count; ++i)
            {
                const size_t at = position + static_cast<size_t> (i);
                inLeft[static_cast<size_t> (i)] = input && at < input->left.size () ? input->left[at] : 0.0f;
                inRight[static_cast<size_t> (i)] = input && at < input->right.size () ? input->right[at] : 0.0f;
            }
            std::fill (outLeft.begin (), outLeft.end (), 0.0f);
            std::fill (outRight.begin (), outRight.end (), 0.0f);

            float* inputs[2] = { inLeft.data (), inRight.data () };
            float* outputs[2] = { outLeft.data (), outRight.data () };

            Vst::AudioBusBuffers inputBus {};
            inputBus.numChannels = 2;
            inputBus.channelBuffers32 = inputs;
            Vst::AudioBusBuffers outputBus {};
            outputBus.numChannels = 2;
            outputBus.channelBuffers32 = outputs;

            _events.events.clear ();
            for (const auto& timed : events)
            {
                if (timed.position >= position && timed.position < position + static_cast<size_t> (count))
                {
                    Vst::Event event = timed.event;
                    event.sampleOffset = static_cast<int32> (timed.position - position);
                    _events.events.push_back (event);
                }
            }

            Vst::ProcessData data {};
            data.processMode = Vst::kRealtime;
            data.symbolicSampleSize = Vst::kSample32;
            data.numSamples = count;
            data.numInputs = hasAudioInput ? 1 : 0;
            data.inputs = hasAudioInput ? &inputBus : nullptr;
            data.numOutputs = 1;
            data.outputs = &outputBus;
            data.inputParameterChanges = _changes.queues.empty () ? nullptr : &_changes;
            data.inputEvents = hasEventInput ? &_events : nullptr;
            data.processContext = context;

            const tresult processed = processor->process (data);
            _changes.queues.clear ();

            for (int i = 0; i < count; ++i)
            {
                const size_t at = (position + static_cast<size_t> (i)) * 2;
                result.interleaved[at] = outLeft[static_cast<size_t> (i)];
                result.interleaved[at + 1] = outRight[static_cast<size_t> (i)];
            }

            char flags[64];
            std::snprintf (flags, sizeof (flags), "%d %d %llu\n", count, static_cast<int> (processed), static_cast<unsigned long long> (outputBus.silenceFlags));
            result.flags += flags;

            if (context && (context->state & Vst::ProcessContext::kPlaying) != 0)
            {
                context->projectTimeMusic += count * context->tempo / (60.0 * _sampleRate);
                context->projectTimeSamples += count;
                const double bar = context->timeSigNumerator * 4.0 / context->timeSigDenominator;
                context->barPositionMusic = std::floor (context->projectTimeMusic / bar) * bar;
            }

            position += static_cast<size_t> (count);
        }
        return result;
    }

    void addChange (Vst::ParamID id, int32 offset, double first, int32 lastOffset, double last)
    {
        controller->setParamNormalized (id, last);
        ValueQueue queue (id);
        queue.add (offset, first);
        queue.add (lastOffset, last);
        _changes.queues.push_back (queue);
    }

    std::vector<char> getState ()
    {
        MemoryStream stream;
        component->getState (&stream);
        return std::vector<char> (stream.getData (), stream.getData () + stream.getSize ());
    }

    // Returns what each side said, for the record.
    std::string setState (const std::vector<char>& bytes)
    {
        MemoryStream forComponent;
        MemoryStream forController;
        int32 written = 0;
        int64 position = 0;
        if (!bytes.empty ())
        {
            forComponent.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
            forController.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
        }
        forComponent.seek (0, IBStream::kIBSeekSet, &position);
        forController.seek (0, IBStream::kIBSeekSet, &position);
        const tresult a = component->setState (&forComponent);
        const tresult b = controller->setComponentState (&forController);
        return "component " + std::to_string (a) + " controller " + std::to_string (b) + "\n";
    }

    Vst::IComponent* component { nullptr };
    Vst::IAudioProcessor* processor { nullptr };
    Vst::IEditController* controller { nullptr };
    Vst::ProcessContext* context { nullptr };
    std::vector<Vst::ParameterInfo> params;
    bool hasAudioInput { false };
    bool hasEventInput { false };

private:
    double _sampleRate;
    bool _isActive { false };
    ParameterChanges _changes;
    EventList _events;
    Vst::IConnectionPoint* _componentConnection { nullptr };
    Vst::IConnectionPoint* _controllerConnection { nullptr };
};

// Tones and noise bursts with gaps of silence, different on each channel.
Input makeSignal (uint32_t seed, size_t count, double sampleRate)
{
    Input input;
    input.left.resize (count);
    input.right.resize (count);

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
        input.left[i] = static_cast<float> (envelope * (tones + noise));
        input.right[i] = static_cast<float> (envelope * (0.8 * tones + 0.2 * std::sin (2.0 * kPi * a * 1.5 * t) - noise));
    }
    return input;
}

std::vector<TimedEvent> makeNotes (uint32_t seed, size_t count, double sampleRate)
{
    std::vector<TimedEvent> events;
    Random random (seed);
    size_t position = 100;
    const auto stepSamples = static_cast<size_t> (0.3 * sampleRate);
    int previousPitch = -1;
    while (position < count)
    {
        const int pitch = 36 + static_cast<int> (random.unit () * 60.0);

        Vst::Event on {};
        on.type = Vst::Event::kNoteOnEvent;
        on.noteOn.channel = static_cast<int16> (random.next () % 16);
        on.noteOn.pitch = static_cast<int16> (pitch);
        on.noteOn.velocity = 0.2f + 0.8f * static_cast<float> (random.unit ());
        on.noteOn.noteId = -1;
        events.push_back ({ position, on });

        // Sometimes legato, by letting go of the last note after the new one starts.
        if (previousPitch >= 0)
        {
            Vst::Event off {};
            off.type = Vst::Event::kNoteOffEvent;
            off.noteOff.pitch = static_cast<int16> (previousPitch);
            off.noteOff.noteId = -1;
            const size_t release = random.unit () < 0.5 ? position + 50 : position - 50;
            events.push_back ({ release, off });
        }
        previousPitch = pitch;
        position += stepSamples + random.next () % stepSamples;
    }

    Vst::Event off {};
    off.type = Vst::Event::kNoteOffEvent;
    off.noteOff.pitch = static_cast<int16> (previousPitch);
    off.noteOff.noteId = -1;
    events.push_back ({ count > 1000 ? count - 1000 : 0, off });
    return events;
}

void setRandom (Plug& plug, uint32_t seed)
{
    Random random (seed * 7919u + 1u);
    plug.setAllAutomatable ([&random] (const Vst::ParameterInfo&) { return random.unit (); });
}

Vst::ProcessContext makeContext (double tempo, double sampleRate, bool playing)
{
    Vst::ProcessContext context {};
    context.state = Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kTempoValid
                    | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kBarPositionValid;
    if (playing)
        context.state |= Vst::ProcessContext::kPlaying;
    context.sampleRate = sampleRate;
    context.tempo = tempo;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;
    return context;
}

void saveOutput (const std::string& name, const Output& output)
{
    writeFile (name + ".f32", output.interleaved.data (), output.interleaved.size () * sizeof (float));
    writeText (name + ".flags", output.flags);
}

void saveBytes (const std::string& name, const std::vector<char>& bytes)
{
    writeFile (name, bytes.data (), bytes.size ());
}

void dumpClasses ()
{
    std::string text;
    PFactoryInfo factoryInfo;
    gFactory->getFactoryInfo (&factoryInfo);
    text += std::string ("vendor ") + factoryInfo.vendor + " url " + factoryInfo.url + " email " + factoryInfo.email + " flags " + std::to_string (factoryInfo.flags) + "\n";

    IPluginFactory2* factory2 = nullptr;
    gFactory->queryInterface (IPluginFactory2::iid, reinterpret_cast<void**> (&factory2));

    for (int32 i = 0; i < gFactory->countClasses (); ++i)
    {
        PClassInfo info;
        gFactory->getClassInfo (i, &info);
        text += hexId (info.cid) + " " + info.category + " " + info.name + " " + std::to_string (info.cardinality) + "\n";
        if (factory2)
        {
            PClassInfo2 info2;
            factory2->getClassInfo2 (i, &info2);
            text += std::string ("  sub ") + info2.subCategories + " vendor " + info2.vendor + " version " + info2.version + " sdk " + info2.sdkVersion + " flags " + std::to_string (info2.classFlags) + "\n";
        }
    }
    if (factory2)
        factory2->release ();
    writeText ("classes.txt", text);
}

void dumpBuses ()
{
    std::string text;
    {
        Plug plug;
        for (auto type : { Vst::kAudio, Vst::kEvent })
        {
            for (auto direction : { Vst::kInput, Vst::kOutput })
            {
                const int32 count = plug.component->getBusCount (type, direction);
                for (int32 i = 0; i < count; ++i)
                {
                    Vst::BusInfo info {};
                    plug.component->getBusInfo (type, direction, i, info);
                    text += "bus " + std::to_string (type) + " " + std::to_string (direction) + " " + narrow (info.name) + " channels " + std::to_string (info.channelCount) + " type " + std::to_string (info.busType) + " flags " + std::to_string (info.flags) + "\n";
                }
            }
        }
        text += "sample32 " + std::to_string (plug.processor->canProcessSampleSize (Vst::kSample32)) + " sample64 " + std::to_string (plug.processor->canProcessSampleSize (Vst::kSample64)) + "\n";
    }

    for (const double rate : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        Plug plug (rate);
        text += "rate " + std::to_string (rate) + " latency " + std::to_string (plug.processor->getLatencySamples ()) + " tail " + std::to_string (plug.processor->getTailSamples ()) + "\n";
    }

    {
        Plug plug (48000.0, false);
        Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
        Vst::SpeakerArrangement mono = Vst::SpeakerArr::kMono;
        text += "arrangement 1:stereo 1:stereo " + std::to_string (plug.processor->setBusArrangements (&stereo, 1, &stereo, 1)) + "\n";
        text += "arrangement 1:mono 1:mono " + std::to_string (plug.processor->setBusArrangements (&mono, 1, &mono, 1)) + "\n";
        text += "arrangement 1:mono 1:stereo " + std::to_string (plug.processor->setBusArrangements (&mono, 1, &stereo, 1)) + "\n";
        text += "arrangement 0 1:stereo " + std::to_string (plug.processor->setBusArrangements (nullptr, 0, &stereo, 1)) + "\n";
        text += "arrangement 0 1:mono " + std::to_string (plug.processor->setBusArrangements (nullptr, 0, &mono, 1)) + "\n";
    }
    writeText ("buses.txt", text);
}

void dumpParameters ()
{
    Plug plug;
    std::string text;
    text += "count " + std::to_string (plug.params.size ()) + "\n";

    for (const auto& info : plug.params)
    {
        text += "param " + std::to_string (info.id) + " title [" + narrow (info.title) + "] short [" + narrow (info.shortTitle) + "] units [" + narrow (info.units) + "] steps " + std::to_string (info.stepCount)
                + " default " + exact (info.defaultNormalizedValue) + " unit " + std::to_string (info.unitId) + " flags " + std::to_string (info.flags)
                + " current " + exact (plug.controller->getParamNormalized (info.id)) + "\n";

        // Every step of a stepped parameter, and a fine sweep of the others. Plus just around the ends.
        std::vector<double> values;
        const int steps = info.stepCount > 0 ? info.stepCount : 200;
        for (int i = 0; i <= steps; ++i)
            values.push_back (static_cast<double> (i) / steps);
        values.push_back (1e-6);
        values.push_back (1.0 - 1e-6);
        values.push_back (0.49999);
        values.push_back (0.50001);

        for (const double value : values)
        {
            Vst::String128 string {};
            const tresult toResult = plug.controller->getParamStringByValue (info.id, value, string);
            Vst::ParamValue back = -1.0;
            const tresult fromResult = plug.controller->getParamValueByString (info.id, string, back);
            const double plain = plug.controller->normalizedParamToPlain (info.id, value);
            const double normalizedAgain = plug.controller->plainParamToNormalized (info.id, plain);
            text += "  " + exact (value) + " plain " + exact (plain) + " again " + exact (normalizedAgain) + " text [" + narrow (string) + "] " + std::to_string (toResult) + " back " + exact (back) + " " + std::to_string (fromResult) + "\n";
        }

        // What typing odd things into a host's value field does.
        for (const char* typed : { "", "-inf", "On", "Off", "abc", "1e9", "-1e9", "nan", "inf", "0", "1", "12.5" })
        {
            Vst::String128 string {};
            size_t i = 0;
            for (; typed[i] != 0; ++i)
                string[i] = static_cast<Vst::TChar> (typed[i]);
            Vst::ParamValue back = -1.0;
            const tresult result = plug.controller->getParamValueByString (info.id, string, back);
            text += std::string ("  typed [") + typed + "] " + std::to_string (result) + " " + exact (back) + "\n";
        }

        // Past the ends of the range.
        text += "  outside " + exact (plug.controller->normalizedParamToPlain (info.id, -0.5)) + " " + exact (plug.controller->normalizedParamToPlain (info.id, 1.5))
                + " " + exact (plug.controller->plainParamToNormalized (info.id, -1e12)) + " " + exact (plug.controller->plainParamToNormalized (info.id, 1e12)) + "\n";
    }

    Vst::ParameterInfo missing {};
    text += "missing index " + std::to_string (plug.controller->getParameterInfo (static_cast<int32> (plug.params.size ()), missing)) + "\n";
    writeText ("parameters.txt", text);
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

void dumpStates ()
{
    {
        Plug plug;
        saveBytes ("state-default.bin", plug.getState ());
    }

    std::vector<int32_t> ids;
    {
        Plug plug;
        for (const auto& info : plug.params)
            ids.push_back (static_cast<int32_t> (info.id));
    }

    const auto allAt = [&ids] (double value) {
        std::vector<std::pair<int32_t, double>> pairs;
        for (const auto id : ids)
            pairs.push_back ({ id, value });
        return pairs;
    };
    const auto spread = [&ids] () {
        std::vector<std::pair<int32_t, double>> pairs;
        Random random (99);
        for (const auto id : ids)
            pairs.push_back ({ id, random.unit () });
        return pairs;
    };

    const auto count = static_cast<int32_t> (ids.size ());
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    const double infinity = std::numeric_limits<double>::infinity ();

    std::vector<std::pair<std::string, std::vector<char>>> cases;
    for (int32_t version : { -1, 0, 1, 2, 3, 4, 99 })
        cases.push_back ({ "version" + std::to_string (version), craftState (version, count, spread ()) });

    auto oneNan = allAt (0.25);
    oneNan[0].second = nan;
    auto oneInfinity = allAt (0.25);
    oneInfinity[0].second = infinity;
    auto outOfRange = allAt (0.25);
    outOfRange[0].second = 2.0;
    outOfRange[1].second = -2.0;
    auto unknown = allAt (0.75);
    unknown.insert (unknown.begin () + 1, { 99999, 0.5 });
    unknown.insert (unknown.begin (), { -5, 0.5 });

    cases.push_back ({ "nan", craftState (1, count, oneNan) });
    cases.push_back ({ "infinity", craftState (1, count, oneInfinity) });
    cases.push_back ({ "out-of-range", craftState (1, count, outOfRange) });
    cases.push_back ({ "unknown-ids", craftState (1, static_cast<int32_t> (unknown.size ()), unknown) });
    cases.push_back ({ "count-too-big", craftState (1, count + 5, allAt (0.25)) });
    cases.push_back ({ "count-negative", craftState (1, -1, allAt (0.25)) });
    cases.push_back ({ "count-zero", craftState (1, 0, {}) });

    auto half = allAt (0.6);
    half.resize (static_cast<size_t> (count / 2));
    cases.push_back ({ "half-of-them", craftState (1, count / 2, half) });

    auto truncated = craftState (1, count, allAt (0.25));
    truncated.resize (truncated.size () - 5);
    cases.push_back ({ "truncated", truncated });
    cases.push_back ({ "only-version", std::vector<char> (4, 1) });
    cases.push_back ({ "empty", {} });

    std::string text;
    for (const auto& [name, bytes] : cases)
    {
        Plug plug;
        text += "case " + name + ": " + plug.setState (bytes);

        for (const auto& info : plug.params)
            text += "  " + std::to_string (info.id) + " " + exact (plug.controller->getParamNormalized (info.id)) + "\n";

        saveBytes ("state-case-" + name + ".bin", plug.getState ());

        // That it survives being played afterwards.
        const auto input = makeSignal (5, 12000, 48000.0);
        const auto notes = plug.hasEventInput ? makeNotes (5, 12000, 48000.0) : std::vector<TimedEvent> {};
        saveOutput ("state-case-" + name, plug.render (12000, &input, { 256 }, notes));
    }
    writeText ("state-cases.txt", text);
}

void renderAudio ()
{
    constexpr size_t kSeconds3 = 144000;

    const auto notesFor = [] (const Plug& plug, uint32_t seed, size_t count, double rate) {
        return plug.hasEventInput ? makeNotes (seed, count, rate) : std::vector<TimedEvent> {};
    };

    // Defaults, at several sample rates.
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        Plug plug (rate);
        const auto total = static_cast<size_t> (3.0 * rate);
        const auto input = makeSignal (1, total, rate);
        saveOutput ("default-" + std::to_string (static_cast<int> (rate)), plug.render (total, &input, { 256 }, notesFor (plug, 1, total, rate)));
    }

    // Spread-out settings.
    for (uint32_t seed = 1; seed <= 12; ++seed)
    {
        Plug plug;
        setRandom (plug, seed);
        const auto input = makeSignal (seed + 10, kSeconds3, 48000.0);
        saveOutput ("seeded-" + std::to_string (seed), plug.render (kSeconds3, &input, { 256 }, notesFor (plug, seed, kSeconds3, 48000.0)));
        saveBytes ("seeded-" + std::to_string (seed) + "-state.bin", plug.getState ());
    }

    // Block sizes, including ones that change as they go.
    const std::vector<std::vector<int>> patterns = { { 32 }, { 64 }, { 1024 }, { 4096 }, { 1 }, { 7, 233, 64, 1, 1000, 511 } };
    for (size_t i = 0; i < patterns.size (); ++i)
    {
        Plug plug;
        setRandom (plug, 3);
        const size_t total = patterns[i][0] == 1 && patterns[i].size () == 1 ? 24000 : kSeconds3;
        const auto input = makeSignal (3, total, 48000.0);
        saveOutput ("blocks-" + std::to_string (i), plug.render (total, &input, patterns[i], notesFor (plug, 3, total, 48000.0)));
    }

    // Both ends of every control.
    for (const double value : { 0.0, 1.0 })
    {
        Plug plug;
        plug.setAllAutomatable ([value] (const Vst::ParameterInfo&) { return value; });
        const auto input = makeSignal (4, kSeconds3, 48000.0);
        saveOutput ("extreme-" + std::to_string (static_cast<int> (value)), plug.render (kSeconds3, &input, { 256 }, notesFor (plug, 4, kSeconds3, 48000.0)));
    }

    // Every control moving, with more than one point in a block.
    for (uint32_t seed = 1; seed <= 3; ++seed)
    {
        Plug plug;
        Random random (seed + 500);
        std::vector<double> rates;
        std::vector<double> phases;
        for (size_t i = 0; i < plug.params.size (); ++i)
        {
            rates.push_back (0.1 + 3.0 * random.unit ());
            phases.push_back (random.unit ());
        }

        const auto input = makeSignal (seed + 20, kSeconds3, 48000.0);
        const auto moving = [&] (size_t position, int count) {
            for (size_t i = 0; i < plug.params.size (); ++i)
            {
                const auto& info = plug.params[i];
                if ((info.flags & Vst::ParameterInfo::kCanAutomate) == 0)
                    continue;
                const auto valueAt = [&] (size_t at) {
                    return 0.5 + 0.5 * std::sin (2.0 * kPi * (rates[i] * static_cast<double> (at) / 48000.0 + phases[i]));
                };
                plug.addChange (info.id, 0, valueAt (position), count - 1, valueAt (position + static_cast<size_t> (count) - 1));
            }
        };
        saveOutput ("moving-" + std::to_string (seed), plug.render (kSeconds3, &input, { 128 }, notesFor (plug, seed, kSeconds3, 48000.0), moving));
    }

    // A host's transport: stopped, playing, tempo changes and locates.
    for (uint32_t seed = 1; seed <= 4; ++seed)
    {
        Plug plug;
        setRandom (plug, seed + 30);
        auto context = makeContext (60.0 + 40.0 * seed, 48000.0, seed != 4);
        plug.context = &context;
        const auto input = makeSignal (seed + 30, kSeconds3, 48000.0);
        const auto transport = [&] (size_t position, int) {
            if (position == 48128)
                context.tempo *= 1.5;
            if (position == 96000)
                context.projectTimeMusic = 16.0;
        };
        saveOutput ("transport-" + std::to_string (seed), plug.render (kSeconds3, &input, { 256 }, notesFor (plug, seed, kSeconds3, 48000.0), transport));
    }

    // A silent start, for the silence flags and sleep logic, and going back to sleep afterwards.
    {
        Plug plug;
        std::vector<float> silent (48000, 0.0f);
        const auto tones = makeSignal (40, 48000, 48000.0);
        Input input;
        input.left = silent;
        input.right = silent;
        input.left.insert (input.left.end (), tones.left.begin (), tones.left.end ());
        input.right.insert (input.right.end (), tones.right.begin (), tones.right.end ());
        saveOutput ("silence", plug.render (480000, &input, { 256 }, plug.hasEventInput ? makeNotes (40, 96000, 48000.0) : std::vector<TimedEvent> {}));
    }

    // Broken samples at the input.
    if (Plug probe; probe.hasAudioInput)
    {
        Plug plug;
        auto input = makeSignal (41, 96000, 48000.0);
        for (size_t i = 24000; i < 24100; ++i)
        {
            input.left[i] = std::numeric_limits<float>::quiet_NaN ();
            input.right[i] = std::numeric_limits<float>::infinity ();
        }
        saveOutput ("broken-input", plug.render (96000, &input, { 256 }));
    }

    // Reactivating in place, and a state moved to another instance.
    {
        Plug plug;
        setRandom (plug, 50);
        const auto input = makeSignal (50, 96000, 48000.0);
        saveOutput ("reactivate-before", plug.render (96000, &input, { 256 }, notesFor (plug, 50, 96000, 48000.0)));
        plug.processor->setProcessing (false);
        plug.component->setActive (false);
        plug.component->setActive (true);
        plug.processor->setProcessing (true);
        saveOutput ("reactivate-after", plug.render (96000, &input, { 256 }, notesFor (plug, 50, 96000, 48000.0)));

        const auto state = plug.getState ();
        saveBytes ("moved-state.bin", state);

        Plug other;
        writeText ("moved-state.txt", other.setState (state));
        saveOutput ("moved-render", other.render (96000, &input, { 256 }, notesFor (other, 50, 96000, 48000.0)));
        saveBytes ("moved-state-again.bin", other.getState ());
    }
}

// LoopTexturer's looper, which needs its buttons pressed. These are the values in src/dsp/Looper.h and src/Messages.h.
constexpr const char* kLooperMessage = "LooperCommand";
constexpr const char* kLooperAttribute = "command";
constexpr int kRec = 1;
constexpr int kOverdub = 2;
constexpr int kPlay = 4;
constexpr int kStop = 8;
constexpr int kClear = 16;

void renderLooper ()
{
    constexpr double kRate = 48000.0;
    const auto seconds = [kRate] (double value) { return static_cast<size_t> (value * kRate); };
    const auto input = makeSignal (60, seconds (8.0), kRate);

    // Free running.
    {
        Plug plug;
        plug.setByTitle ("Looper length", 1.5);
        plug.setByTitle ("Looper dry/wet", 100.0);
        plug.setByTitle ("Looper feedback", 90.0);
        saveOutput ("looper-free-1-settle", plug.render (seconds (0.8), nullptr, { 256 }));

        plug.sendCommand (kLooperMessage, kLooperAttribute, kRec);
        saveOutput ("looper-free-2-record", plug.render (seconds (1.5), &input, { 256 }));
        saveOutput ("looper-free-3-overdub", plug.render (seconds (2.0), &input, { 256 }));
        plug.sendCommand (kLooperMessage, kLooperAttribute, kOverdub);
        saveOutput ("looper-free-4-play", plug.render (seconds (2.0), nullptr, { 256 }));

        // A new speed repitches the loop.
        plug.setByTitle ("Looper length", 3.0);
        saveOutput ("looper-free-5-slower", plug.render (seconds (2.0), nullptr, { 256 }));

        plug.sendCommand (kLooperMessage, kLooperAttribute, kStop);
        saveOutput ("looper-free-6-stop", plug.render (seconds (0.5), nullptr, { 256 }));
        plug.sendCommand (kLooperMessage, kLooperAttribute, kPlay);
        saveOutput ("looper-free-7-play", plug.render (seconds (1.0), nullptr, { 256 }));

        // A saved loop, restored into a new instance.
        const auto state = plug.getState ();
        saveBytes ("looper-free-state.bin", state);
        {
            Plug other;
            writeText ("looper-free-restore.txt", other.setState (state));
            saveOutput ("looper-free-8-restored", other.render (seconds (2.0), nullptr, { 256 }));
            saveBytes ("looper-free-state-again.bin", other.getState ());
        }

        plug.sendCommand (kLooperMessage, kLooperAttribute, kClear);
        saveOutput ("looper-free-9-cleared", plug.render (seconds (0.5), nullptr, { 256 }));
    }

    // Tempo sync. The mode change happens in a parameter, and the worker thread allocates the tape afterwards.
    {
        Plug plug;
        auto context = makeContext (120.0, kRate, false);
        plug.context = &context;
        plug.setByTitle ("Looper mode", 1);
        plug.setByTitle ("Looper dry/wet", 100.0);
        plug.setByTitle ("Looper feedback", 80.0);
        plug.setByTitle ("Looper bars", 1); // 1 bar.
        saveOutput ("looper-sync-1-settle", plug.render (seconds (0.3), nullptr, { 256 }));

        // The tape arrives when it arrives, and nothing is playing or recording until then.
        std::this_thread::sleep_for (std::chrono::seconds (3));
        plug.render (seconds (0.1), nullptr, { 256 });

        context.state |= Vst::ProcessContext::kPlaying;
        plug.sendCommand (kLooperMessage, kLooperAttribute, kRec);
        saveOutput ("looper-sync-2-record", plug.render (seconds (4.0), &input, { 256 }));
        saveOutput ("looper-sync-3-overdub", plug.render (seconds (4.0), &input, { 256 }));
        plug.sendCommand (kLooperMessage, kLooperAttribute, kOverdub);
        saveOutput ("looper-sync-4-play", plug.render (seconds (4.0), nullptr, { 256 }));

        const auto state = plug.getState ();
        saveBytes ("looper-sync-state.bin", state);
        {
            Plug other;
            auto otherContext = makeContext (120.0, kRate, true);
            other.context = &otherContext;
            writeText ("looper-sync-restore.txt", other.setState (state));
            saveOutput ("looper-sync-5-restored", other.render (seconds (4.0), nullptr, { 256 }));
        }

        context.tempo = 90.0;
        saveOutput ("looper-sync-6-tempo", plug.render (seconds (4.0), nullptr, { 256 }));
        context.state &= ~Vst::ProcessContext::kPlaying;
    }
}

} // namespace

#if defined(_WIN32)
int wmain (int argc, wchar_t** argv)
#else
int main (int argc, char** argv)
#endif
{
    if (argc != 3)
    {
        std::printf ("Usage: NullTest <path to a plug-in's DLL or .so> <output folder>\n");
        return 2;
    }

    gOutput = argv[2];
    fs::create_directories (gOutput);

#if defined(_WIN32)
    HMODULE module = LoadLibraryW (argv[1]);
    if (!module)
    {
        std::wprintf (L"Could not load: %ls\n", argv[1]);
        return 2;
    }

    auto initDll = reinterpret_cast<bool (*) ()> (GetProcAddress (module, "InitDll"));
    auto exitDll = reinterpret_cast<bool (*) ()> (GetProcAddress (module, "ExitDll"));
    auto getPluginFactory = reinterpret_cast<GetFactoryProc> (GetProcAddress (module, "GetPluginFactory"));
    const bool entered = initDll && exitDll && getPluginFactory && initDll ();
#else
    void* module = dlopen (argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!module)
    {
        std::printf ("Could not load: %s\n", dlerror ());
        return 2;
    }

    auto moduleEntry = reinterpret_cast<bool (*) (void*)> (dlsym (module, "ModuleEntry"));
    auto exitDll = reinterpret_cast<bool (*) ()> (dlsym (module, "ModuleExit"));
    auto getPluginFactory = reinterpret_cast<GetFactoryProc> (dlsym (module, "GetPluginFactory"));
    const bool entered = moduleEntry && exitDll && getPluginFactory && moduleEntry (module);
#endif
    if (!entered)
    {
        std::printf ("Not a VST3 module\n");
        return 2;
    }

    gFactory = getPluginFactory ();
    for (int32 i = 0; i < gFactory->countClasses (); ++i)
    {
        PClassInfo info;
        gFactory->getClassInfo (i, &info);
        if (std::strcmp (info.category, kVstAudioEffectClass) == 0)
            std::memcpy (gComponentId, info.cid, sizeof (TUID));
        else if (std::strcmp (info.category, kVstComponentControllerClass) == 0)
            std::memcpy (gControllerId, info.cid, sizeof (TUID));
    }

    const auto stage = [] (const char* name, void (*function) ()) {
        std::printf ("%s\n", name);
        std::fflush (stdout);
        function ();
    };

    stage ("classes", dumpClasses);
    stage ("buses", dumpBuses);
    stage ("parameters", dumpParameters);
    stage ("states", dumpStates);
    stage ("audio", renderAudio);

    bool hasLooper = false;
    {
        Plug plug;
        hasLooper = plug.hasParam ("Looper mode");
    }
    if (hasLooper)
        stage ("looper", renderLooper);

    gFactory->release ();
    exitDll ();
#if defined(_WIN32)
    FreeLibrary (module);
#else
    dlclose (module);
#endif
    std::printf ("done\n");
    return 0;
}
