// Runs a plug-in's own VST3 processor behind an LV2 interface, in the same process and with no changes to it, so that
// the two can only differ if this file gets something wrong. It has audio, MIDI, control ports, saved state, latency
// and transport, and Ui.cpp has the panel. tools/lv2compare compares its output with the VST3's, bit for bit.
//
// With message ports, it also carries the VST3 messages between the processor and the panel, which may be in another
// process. run () only copies bytes through a queue each way, and a thread of the instance's own delivers the panel's
// messages to the processor, as a VST3 host's UI thread would.

#include "ByteQueue.h"
#include "Common.h"
#include "MessageAtoms.h"

#include "hadronyche/lv2/Message.h"
#include "hadronyche/lv2/MessageLink.h"
#include "hadronyche/lv2/PluginInfo.h"
#include "hadronyche/vst/ParamState.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;

namespace Hadronyche {
namespace Lv2 {

namespace {

// More notes than this in one block are dropped, because run () can't allocate.
constexpr int kMaxEvents = 1024;

constexpr int kStereo = 2;

// A message larger than this is dropped. LoopTexturer's largest is about 2 kB.
constexpr uint32_t kMaxMessage = 1 << 16;
constexpr size_t kToProcessorBytes = 1 << 18;
constexpr size_t kToPanelBytes = 1 << 20;

constexpr auto kMessagePoll = std::chrono::milliseconds (10);
constexpr auto kTickInterval = std::chrono::milliseconds (33);

#define LV2WRAPPER_NO_REFCOUNT                                                                \
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }  \
    uint32 PLUGIN_API addRef () override { return 1; }                                        \
    uint32 PLUGIN_API release () override { return 1; }

// A control port has one value per run (), which ParamStore::readChanges treats like a point at offset 0.
class OnePointQueue : public Vst::IParamValueQueue
{
public:
    LV2WRAPPER_NO_REFCOUNT

    void set (Vst::ParamID id, double normalized)
    {
        _id = id;
        _normalized = normalized;
    }

    Vst::ParamID PLUGIN_API getParameterId () override { return _id; }
    int32 PLUGIN_API getPointCount () override { return 1; }

    tresult PLUGIN_API getPoint (int32 index, int32& offset, Vst::ParamValue& value) override
    {
        if (index != 0)
            return kResultFalse;
        offset = 0;
        value = _normalized;
        return kResultTrue;
    }

    tresult PLUGIN_API addPoint (int32, Vst::ParamValue, int32&) override { return kResultFalse; }

private:
    Vst::ParamID _id { 0 };
    double _normalized { 0.0 };
};

class Changes : public Vst::IParameterChanges
{
public:
    LV2WRAPPER_NO_REFCOUNT

    void prepare (uint32_t paramCount) { _queues.resize (paramCount); }
    void clear () { _count = 0; }
    void add (Vst::ParamID id, double normalized) { _queues[static_cast<size_t> (_count++)].set (id, normalized); }

    int32 PLUGIN_API getParameterCount () override { return _count; }

    Vst::IParamValueQueue* PLUGIN_API getParameterData (int32 index) override
    {
        return index >= 0 && index < _count ? &_queues[static_cast<size_t> (index)] : nullptr;
    }

    Vst::IParamValueQueue* PLUGIN_API addParameterData (const Vst::ParamID&, int32&) override { return nullptr; }

private:
    std::vector<OnePointQueue> _queues;
    int32 _count { 0 };
};

class Events : public Vst::IEventList
{
public:
    LV2WRAPPER_NO_REFCOUNT

    void clear () { _count = 0; }

    void add (const Vst::Event& event)
    {
        if (_count < kMaxEvents)
            _events[static_cast<size_t> (_count++)] = event;
    }

    int32 PLUGIN_API getEventCount () override { return _count; }

    tresult PLUGIN_API getEvent (int32 index, Vst::Event& event) override
    {
        if (index < 0 || index >= _count)
            return kResultFalse;
        event = _events[static_cast<size_t> (index)];
        return kResultOk;
    }

    tresult PLUGIN_API addEvent (Vst::Event&) override { return kResultFalse; }

private:
    std::array<Vst::Event, kMaxEvents> _events {};
    int32 _count { 0 };
};

// Hosts give these as different types of number.
bool readNumber (const LV2_Atom* atom, LV2_URID floatType, LV2_URID doubleType, LV2_URID intType, LV2_URID longType, double& value)
{
    if (!atom)
        return false;
    if (atom->type == floatType)
        value = reinterpret_cast<const LV2_Atom_Float*> (atom)->body;
    else if (atom->type == doubleType)
        value = reinterpret_cast<const LV2_Atom_Double*> (atom)->body;
    else if (atom->type == intType)
        value = reinterpret_cast<const LV2_Atom_Int*> (atom)->body;
    else if (atom->type == longType)
        value = static_cast<double> (reinterpret_cast<const LV2_Atom_Long*> (atom)->body);
    else
        return false;
    return std::isfinite (value);
}

class Instance : private Vst::IConnectionPoint, private MessageSender
{
public:
    explicit Instance (const PluginInfo& info) : _info (info), _layout (getPortLayout (info)) {}

    ~Instance ()
    {
        stopMessages ();
        if (_processor)
            _processor->release ();
        if (_component)
        {
            _component->terminate ();
            _component->release ();
        }
    }

    bool create (double sampleRate, int32 maxBlock, LV2_URID_Map* map)
    {
        _sampleRate = sampleRate;
        _context.tempo = 120.0;
        _context.timeSigNumerator = 4;
        _context.timeSigDenominator = 4;
        readUrids (map);

        auto* factory = GetPluginFactory ();
        if (!factory)
            return false;

        TUID componentId {};
        bool isFound = false;
        for (int32 i = 0; i < factory->countClasses () && !isFound; ++i)
        {
            PClassInfo classInfo;
            if (factory->getClassInfo (i, &classInfo) == kResultOk && std::strcmp (classInfo.category, kVstAudioEffectClass) == 0)
            {
                std::memcpy (componentId, classInfo.cid, sizeof (TUID));
                isFound = true;
            }
        }
        const bool isCreated = isFound && factory->createInstance (componentId, Vst::IComponent::iid, reinterpret_cast<void**> (&_component)) == kResultOk;
        factory->release ();
        if (!isCreated || _component->initialize (&_hostContext) != kResultOk)
            return false;
        if (_component->queryInterface (Vst::IAudioProcessor::iid, reinterpret_cast<void**> (&_processor)) != kResultOk)
            return false;
        if (_info.hasMessagePorts && !startMessages (map))
            return false;

        Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, maxBlock, sampleRate };
        if (_processor->setupProcessing (setup) != kResultOk)
            return false;
        if (_info.hasAudioInput)
            _component->activateBus (Vst::kAudio, Vst::kInput, 0, true);
        _component->activateBus (Vst::kAudio, Vst::kOutput, 0, true);
        if (_info.hasMidiInput)
            _component->activateBus (Vst::kEvent, Vst::kInput, 0, true);

        _changes.prepare (_info.paramCount);
        _controls.assign (_info.paramCount, nullptr);
        _seenControls.resize (_info.paramCount);
        _appliedControls.resize (_info.paramCount);
        for (auto& channel : _inputCopies)
            channel.resize (static_cast<size_t> (maxBlock));

        // The processor starts on its defaults, so a port holding its default doesn't count as a change. That leaves
        // those parameters at the exact value from ParamStore, instead of one that went through a float.
        for (Vst::ParamID id = 0; id < _info.paramCount; ++id)
        {
            _seenControls[id] = static_cast<float> (_info.getSpec (id).defaultValue);
            _appliedControls[id] = _seenControls[id];
        }
        return true;
    }

    void connect (uint32_t port, void* data)
    {
        if (_info.hasAudioInput && port - _layout.audioInput < kStereo)
            _inputs[port - _layout.audioInput] = static_cast<float*> (data);
        else if (port - _layout.audioOutput < kStereo)
            _outputs[port - _layout.audioOutput] = static_cast<float*> (data);
        else if (port == _layout.events)
            _eventInput = static_cast<const LV2_Atom_Sequence*> (data);
        else if (port == _layout.latency)
            _latencyOutput = static_cast<float*> (data);
        else if (port == _layout.messageInput)
            _messageInput = static_cast<const LV2_Atom_Sequence*> (data);
        else if (port == _layout.messageOutput)
            _messageOutput = static_cast<LV2_Atom_Sequence*> (data);
        else if (port - _layout.firstControl < _info.paramCount)
            _controls[port - _layout.firstControl] = static_cast<const float*> (data);
    }

    void activate ()
    {
        _component->setActive (true);
        _processor->setProcessing (true);
    }

    void deactivate ()
    {
        _processor->setProcessing (false);
        _component->setActive (false);
    }

    void run (uint32_t sampleCount)
    {
        readControls ();
        readEvents ();
        readMessages ();

        Vst::AudioBusBuffers output {};
        output.numChannels = kStereo;
        output.channelBuffers32 = _outputs.data ();

        Vst::ProcessData data {};
        data.processMode = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples = static_cast<int32> (sampleCount);
        data.numOutputs = 1;
        data.outputs = &output;
        data.inputParameterChanges = &_changes;
        data.inputEvents = _info.hasMidiInput ? &_events : nullptr;
        data.processContext = _hasContext ? &_context : nullptr;

        Vst::AudioBusBuffers input {};
        std::array<float*, kStereo> inputs {};
        if (_info.hasAudioInput)
        {
            // LV2 lets a host use one buffer for an input and an output, which the VST3 processors aren't written for.
            for (size_t c = 0; c < kStereo; ++c)
            {
                inputs[c] = _inputs[c];
                if (_inputs[c] == _outputs[0] || _inputs[c] == _outputs[1])
                {
                    std::copy_n (_inputs[c], sampleCount, _inputCopies[c].data ());
                    inputs[c] = _inputCopies[c].data ();
                }
            }
            input.numChannels = kStereo;
            input.channelBuffers32 = inputs.data ();
            data.numInputs = 1;
            data.inputs = &input;
        }

        _processor->process (data);

        if (_latencyOutput)
            *_latencyOutput = static_cast<float> (_processor->getLatencySamples ());

        advanceTransport (sampleCount);
        writeMessages ();
    }

    LV2_State_Status save (LV2_State_Store_Function store, LV2_State_Handle handle)
    {
        MemoryStream stream;
        if (_component->getState (&stream) != kResultOk)
            return LV2_STATE_ERR_UNKNOWN;

        return store (handle, _stateKey, stream.getData (), static_cast<size_t> (stream.getSize ()), _atomChunk,
                      LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }

    LV2_State_Status restore (LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle)
    {
        size_t size = 0;
        uint32_t type = 0;
        uint32_t flags = 0;
        const void* bytes = retrieve (handle, _stateKey, &size, &type, &flags);
        if (!bytes || type != _atomChunk)
            return LV2_STATE_ERR_NO_PROPERTY;

        MemoryStream stream;
        int32 written = 0;
        int64 position = 0;
        stream.write (const_cast<void*> (bytes), static_cast<int32> (size), &written);
        stream.seek (0, IBStream::kIBSeekSet, &position);
        const tresult result = _component->setState (&stream);

        // Hosts restore the ports too, in either order, or not at all. A port holding what this state rounds to isn't a
        // new value, and neither is one that hasn't moved, so that neither replaces the exact value from the state.
        MemoryStream current;
        if (_component->getState (&current) == kResultOk)
        {
            current.seek (0, IBStream::kIBSeekSet, &position);
            readParamState (&current, std::numeric_limits<int32>::max (), static_cast<int32> (_info.paramCount),
                            [this] (Vst::ParamID id, double normalized, int32) {
                                _appliedControls[id] = static_cast<float> (normalizedToPlain (_info.getSpec (id), normalized));
                            });
        }
        return result == kResultOk ? LV2_STATE_SUCCESS : LV2_STATE_ERR_BAD_TYPE;
    }

private:
    // IConnectionPoint, as the processor's peer, which gets what the processor sends its controller.
    tresult PLUGIN_API connect (Vst::IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API disconnect (Vst::IConnectionPoint*) override { return kResultOk; }

    tresult PLUGIN_API notify (Vst::IMessage* message) override
    {
        if (!message)
            return kInvalidArgument;

        const std::lock_guard<std::recursive_mutex> lock (_linkMutex);
        if (!_link || !_link->fromProcessor (message))
            send (message);
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Vst::IConnectionPoint::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<Vst::IConnectionPoint*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    // MessageSender, to the panel. Only messages from the wrapper's own HostContext can be read back into bytes, and a
    // processor's allocateMessage gives those.
    void send (Vst::IMessage* message) override
    {
        auto* readable = dynamic_cast<Message*> (message);
        if (!readable)
            return;

        const auto bytes = readable->toBytes ();
        if (bytes.size () > kMaxMessage)
            return;

        const std::lock_guard<std::mutex> lock (_toPanelMutex);
        _toPanel->push (bytes.data (), static_cast<uint32_t> (bytes.size ()));
    }

    bool startMessages (LV2_URID_Map* map)
    {
        _atoms.map (map, _info);
        _outgoing.resize (kMaxMessage);
        _toProcessor = std::make_unique<ByteQueue> (kToProcessorBytes);
        _toPanel = std::make_unique<ByteQueue> (kToPanelBytes);
        _link = createDspLink ();

        // The processor introduces itself to its peer as it connects, so the link has to exist by then.
        if (_component->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_processorConnection)) != kResultOk)
            return false;
        if (_processorConnection->connect (this) != kResultOk)
            return false;

        _messageThread = std::thread ([this] { runMessages (); });
        return true;
    }

    void stopMessages ()
    {
        if (_messageThread.joinable ())
        {
            {
                const std::lock_guard<std::mutex> lock (_stopMutex);
                _isStopping = true;
            }
            _stopWake.notify_all ();
            _messageThread.join ();
        }
        if (_processorConnection)
        {
            _processorConnection->disconnect (this);
            _processorConnection->release ();
            _processorConnection = nullptr;
        }

        const std::lock_guard<std::recursive_mutex> lock (_linkMutex);
        _link.reset ();
    }

    void runMessages ()
    {
        auto nextTick = std::chrono::steady_clock::now ();
        std::vector<uint8_t> bytes;
        std::unique_lock<std::mutex> stopLock (_stopMutex);
        while (!_isStopping)
        {
            _stopWake.wait_for (stopLock, kMessagePoll);
            if (_isStopping)
                break;
            stopLock.unlock ();

            uint32_t size = 0;
            while (_toProcessor->peekSize (size))
            {
                bytes.resize (size);
                _toProcessor->pop (bytes.data ());
                if (auto* message = Message::fromBytes (bytes.data (), bytes.size ()))
                {
                    const std::lock_guard<std::recursive_mutex> lock (_linkMutex);
                    if (!_link || !_link->fromPanel (message, *this))
                        _processorConnection->notify (message);
                    message->release ();
                }
            }

            const auto now = std::chrono::steady_clock::now ();
            if (now >= nextTick)
            {
                nextTick = now + kTickInterval;
                const std::lock_guard<std::recursive_mutex> lock (_linkMutex);
                if (_link)
                    _link->tick (*this);
            }
            stopLock.lock ();
        }
    }

    // Audio thread: whatever doesn't fit in the queue is dropped.
    void readMessages ()
    {
        if (!_messageInput)
            return;

        LV2_ATOM_SEQUENCE_FOREACH (_messageInput, event)
        {
            const void* bytes = nullptr;
            uint32_t size = 0;
            if (_atoms.read (&event->body, static_cast<uint32_t> (sizeof (LV2_Atom)) + event->body.size, bytes, size) && size <= kMaxMessage)
                _toProcessor->push (bytes, size);
        }
    }

    // Audio thread. The host sets the port's size to its room before each run (). A message that doesn't fit waits for
    // the next one, unless it never could.
    void writeMessages ()
    {
        if (!_messageOutput)
            return;

        const uint32_t room = _messageOutput->atom.size;
        if (room < sizeof (LV2_Atom_Sequence_Body))
            return;

        _messageOutput->atom.type = _atomSequence;
        _messageOutput->atom.size = sizeof (LV2_Atom_Sequence_Body);
        _messageOutput->body.unit = 0;
        _messageOutput->body.pad = 0;

        uint32_t size = 0;
        while (_toPanel->peekSize (size))
        {
            // The event's time, then the atom with its header, which LV2_Atom_Event already counts.
            const uint32_t eventSize = lv2_atom_pad_size (static_cast<uint32_t> (sizeof (LV2_Atom_Event) - sizeof (LV2_Atom)) + MessageAtoms::atomSize (size));
            if (eventSize > room - sizeof (LV2_Atom_Sequence_Body) || size > kMaxMessage)
            {
                _toPanel->skip ();
                continue;
            }
            if (eventSize > room - _messageOutput->atom.size)
                break;

            _toPanel->pop (_outgoing.data ());
            auto* event = reinterpret_cast<LV2_Atom_Event*> (reinterpret_cast<uint8_t*> (&_messageOutput->body) + _messageOutput->atom.size);
            event->time.frames = 0;
            _atoms.write (&event->body, _outgoing.data (), size);
            _messageOutput->atom.size += eventSize;
        }
    }

    void readUrids (LV2_URID_Map* map)
    {
        const auto get = [map] (const char* uri) { return map->map (map->handle, uri); };
        _midiEvent = get (LV2_MIDI__MidiEvent);
        _atomChunk = get (LV2_ATOM__Chunk);
        _atomObject = get (LV2_ATOM__Object);
        _atomBlank = get (LV2_ATOM__Blank);
        _atomFloat = get (LV2_ATOM__Float);
        _atomDouble = get (LV2_ATOM__Double);
        _atomInt = get (LV2_ATOM__Int);
        _atomLong = get (LV2_ATOM__Long);
        _atomSequence = get (LV2_ATOM__Sequence);
        _timePosition = get (LV2_TIME__Position);
        _timeFrame = get (LV2_TIME__frame);
        _timeSpeed = get (LV2_TIME__speed);
        _timeBpm = get (LV2_TIME__beatsPerMinute);
        _timeBeat = get (LV2_TIME__beat);
        _timeBar = get (LV2_TIME__bar);
        _timeBarBeat = get (LV2_TIME__barBeat);
        _timeBeatsPerBar = get (LV2_TIME__beatsPerBar);
        _timeBeatUnit = get (LV2_TIME__beatUnit);
        _stateKey = get ((std::string (_info.uri) + "#state").c_str ());
    }

    void readControls ()
    {
        _changes.clear ();
        for (Vst::ParamID id = 0; id < _info.paramCount; ++id)
        {
            if (!_controls[id])
                continue;

            const float value = *_controls[id];
            if (value == _seenControls[id] || !std::isfinite (value))
                continue;

            _seenControls[id] = value;
            if (value == _appliedControls[id])
                continue;

            _appliedControls[id] = value;
            _changes.add (id, controlToNormalized (_info.getSpec (id), value));
        }
    }

    void readEvents ()
    {
        _events.clear ();
        if (!_eventInput)
            return;

        LV2_ATOM_SEQUENCE_FOREACH (_eventInput, event)
        {
            if (event->body.type == _midiEvent)
                readMidi (event);
            else if (_info.followsTransport && (event->body.type == _atomObject || event->body.type == _atomBlank))
                readPosition (reinterpret_cast<const LV2_Atom_Object*> (&event->body));
        }
    }

    void readMidi (const LV2_Atom_Event* event)
    {
        if (!_info.hasMidiInput || event->body.size < 3)
            return;

        const auto* message = reinterpret_cast<const uint8_t*> (event + 1);
        const int type = message[0] & 0xF0;
        const auto channel = static_cast<int16> (message[0] & 0x0F);
        const auto pitch = static_cast<int16> (message[1] & 0x7F);
        const float velocity = static_cast<float> (message[2] & 0x7F) / 127.0f;

        Vst::Event out {};
        out.sampleOffset = static_cast<int32> (event->time.frames);
        if (type == LV2_MIDI_MSG_NOTE_ON)
        {
            out.type = Vst::Event::kNoteOnEvent;
            out.noteOn.channel = channel;
            out.noteOn.pitch = pitch;
            out.noteOn.velocity = velocity;
            out.noteOn.noteId = -1;
            _events.add (out);
        }
        else if (type == LV2_MIDI_MSG_NOTE_OFF)
        {
            out.type = Vst::Event::kNoteOffEvent;
            out.noteOff.channel = channel;
            out.noteOff.pitch = pitch;
            out.noteOff.velocity = velocity;
            out.noteOff.noteId = -1;
            _events.add (out);
        }
    }

    // A block has one ProcessContext, so a position part-way through a block applies to all of it. The hosts' usual
    // place for one is at the block's start.
    void readPosition (const LV2_Atom_Object* object)
    {
        if (object->body.otype != _timePosition)
            return;

        const LV2_Atom* frame = nullptr;
        const LV2_Atom* speed = nullptr;
        const LV2_Atom* bpm = nullptr;
        const LV2_Atom* beat = nullptr;
        const LV2_Atom* bar = nullptr;
        const LV2_Atom* barBeat = nullptr;
        const LV2_Atom* beatsPerBar = nullptr;
        const LV2_Atom* beatUnit = nullptr;
        lv2_atom_object_get (object, _timeFrame, &frame, _timeSpeed, &speed, _timeBpm, &bpm, _timeBeat, &beat, _timeBar, &bar,
                             _timeBarBeat, &barBeat, _timeBeatsPerBar, &beatsPerBar, _timeBeatUnit, &beatUnit, 0);

        const auto number = [this] (const LV2_Atom* atom, double& value) {
            return readNumber (atom, _atomFloat, _atomDouble, _atomInt, _atomLong, value);
        };

        double value = 0.0;
        if (number (bpm, value) && value > 0.0)
            _context.tempo = value;
        if (number (beatsPerBar, value) && value > 0.0)
            _context.timeSigNumerator = static_cast<int32> (value);
        if (number (beatUnit, value) && value > 0.0)
            _context.timeSigDenominator = static_cast<int32> (value);
        if (number (speed, value))
            _isPlaying = value != 0.0;
        if (number (frame, value))
            _context.projectTimeSamples = static_cast<int64> (value);

        double beatsInBar = 0.0;
        double barIndex = 0.0;
        if (number (beat, value))
            _context.projectTimeMusic = value;
        else if (number (bar, barIndex) && number (barBeat, beatsInBar))
            _context.projectTimeMusic = barIndex * _context.timeSigNumerator * 4.0 / _context.timeSigDenominator + beatsInBar;

        _context.sampleRate = _sampleRate;
        _hasContext = true;
        updateContextState ();
    }

    void updateContextState ()
    {
        _context.state = Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kTempoValid
                         | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kBarPositionValid;
        if (_isPlaying)
            _context.state |= Vst::ProcessContext::kPlaying;

        const double bar = _context.timeSigNumerator * 4.0 / _context.timeSigDenominator;
        _context.barPositionMusic = std::floor (_context.projectTimeMusic / bar) * bar;
    }

    // A host only has to send a position when something jumps.
    void advanceTransport (uint32_t sampleCount)
    {
        if (!_hasContext || !_isPlaying)
            return;

        _context.projectTimeMusic += sampleCount * _context.tempo / (60.0 * _sampleRate);
        _context.projectTimeSamples += sampleCount;
        updateContextState ();
    }

    const PluginInfo& _info;
    const PortLayout _layout;
    HostContext _hostContext;
    Vst::IComponent* _component { nullptr };
    Vst::IAudioProcessor* _processor { nullptr };
    double _sampleRate { 44100.0 };

    LV2_URID _midiEvent { 0 };
    LV2_URID _atomChunk { 0 };
    LV2_URID _atomObject { 0 };
    LV2_URID _atomBlank { 0 };
    LV2_URID _atomFloat { 0 };
    LV2_URID _atomDouble { 0 };
    LV2_URID _atomInt { 0 };
    LV2_URID _atomLong { 0 };
    LV2_URID _atomSequence { 0 };
    LV2_URID _timePosition { 0 };
    LV2_URID _timeFrame { 0 };
    LV2_URID _timeSpeed { 0 };
    LV2_URID _timeBpm { 0 };
    LV2_URID _timeBeat { 0 };
    LV2_URID _timeBar { 0 };
    LV2_URID _timeBarBeat { 0 };
    LV2_URID _timeBeatsPerBar { 0 };
    LV2_URID _timeBeatUnit { 0 };
    LV2_URID _stateKey { 0 };

    std::array<float*, kStereo> _inputs {};
    std::array<float*, kStereo> _outputs {};
    std::array<std::vector<float>, kStereo> _inputCopies;
    const LV2_Atom_Sequence* _eventInput { nullptr };
    float* _latencyOutput { nullptr };
    std::vector<const float*> _controls;
    std::vector<float> _seenControls;    // What each port last held.
    std::vector<float> _appliedControls; // What the processor has, as a float.

    Changes _changes;
    Events _events;

    // Stays off until a host sends a position, so that a host without transport sees what a VST3 host without it does.
    Vst::ProcessContext _context { };
    bool _hasContext { false };
    bool _isPlaying { false };

    // Message ports. Every link call holds _linkMutex, and producers to the panel take turns with _toPanelMutex, so
    // that each queue has one writer at a time.
    const LV2_Atom_Sequence* _messageInput { nullptr };
    LV2_Atom_Sequence* _messageOutput { nullptr };
    MessageAtoms _atoms;
    std::unique_ptr<ByteQueue> _toProcessor;
    std::unique_ptr<ByteQueue> _toPanel;
    std::vector<uint8_t> _outgoing;
    std::mutex _toPanelMutex;
    std::recursive_mutex _linkMutex; // A processor can answer a message from inside notify.
    std::unique_ptr<DspLink> _link;
    Vst::IConnectionPoint* _processorConnection { nullptr };
    std::thread _messageThread;
    std::mutex _stopMutex;
    std::condition_variable _stopWake;
    bool _isStopping { false };
};

LV2_Handle instantiate (const LV2_Descriptor*, double sampleRate, const char*, const LV2_Feature* const* features)
{
    LV2_URID_Map* map = nullptr;
    const LV2_Options_Option* options = nullptr;
    bool isBlockBounded = false;
    for (int i = 0; features && features[i]; ++i)
    {
        if (std::strcmp (features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<LV2_URID_Map*> (features[i]->data);
        else if (std::strcmp (features[i]->URI, LV2_OPTIONS__options) == 0)
            options = static_cast<const LV2_Options_Option*> (features[i]->data);
        else if (std::strcmp (features[i]->URI, LV2_BUF_SIZE__boundedBlockLength) == 0)
            isBlockBounded = true;
    }
    if (!map || !options || !isBlockBounded)
        return nullptr;

    // setupProcessing needs a maximum, which is why the Turtle requires it.
    const LV2_URID maxBlockKey = map->map (map->handle, LV2_BUF_SIZE__maxBlockLength);
    const LV2_URID atomInt = map->map (map->handle, LV2_ATOM__Int);
    int32 maxBlock = 0;
    for (int i = 0; options[i].key != 0 || options[i].value != nullptr; ++i)
    {
        if (options[i].key == maxBlockKey && options[i].type == atomInt)
            maxBlock = *static_cast<const int32_t*> (options[i].value);
    }
    if (maxBlock <= 0 || !enterModule ())
        return nullptr;

    auto* instance = new (std::nothrow) Instance (getPluginInfo ());
    if (!instance || !instance->create (sampleRate, maxBlock, map))
    {
        delete instance;
        exitModule ();
        return nullptr;
    }
    return instance;
}

void connectPort (LV2_Handle handle, uint32_t port, void* data)
{
    static_cast<Instance*> (handle)->connect (port, data);
}

void activate (LV2_Handle handle)
{
    static_cast<Instance*> (handle)->activate ();
}

void run (LV2_Handle handle, uint32_t sampleCount)
{
    static_cast<Instance*> (handle)->run (sampleCount);
}

void deactivate (LV2_Handle handle)
{
    static_cast<Instance*> (handle)->deactivate ();
}

void cleanup (LV2_Handle handle)
{
    delete static_cast<Instance*> (handle);
    exitModule ();
}

LV2_State_Status saveState (LV2_Handle handle, LV2_State_Store_Function store, LV2_State_Handle stateHandle, uint32_t, const LV2_Feature* const*)
{
    return static_cast<Instance*> (handle)->save (store, stateHandle);
}

LV2_State_Status restoreState (LV2_Handle handle, LV2_State_Retrieve_Function retrieve, LV2_State_Handle stateHandle, uint32_t, const LV2_Feature* const*)
{
    return static_cast<Instance*> (handle)->restore (retrieve, stateHandle);
}

const void* extensionData (const char* uri)
{
    static const LV2_State_Interface stateInterface { saveState, restoreState };
    return std::strcmp (uri, LV2_STATE__interface) == 0 ? &stateInterface : nullptr;
}

} // namespace

} // namespace Lv2
} // namespace Hadronyche

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    using namespace Hadronyche::Lv2;
    static const LV2_Descriptor descriptor { getPluginInfo ().uri, instantiate, connectPort, activate, run, deactivate, cleanup, extensionData };
    return index == 0 ? &descriptor : nullptr;
}
