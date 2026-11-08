#include "Check.h"
#include "ExampleParamIds.h"

#include "hadronyche/vst/RustController.h"
#include "hadronyche/vst/RustProcessor.h"

#include "public.sdk/source/common/memorystream.h"

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// RustProcessor and RustController, driven as a host would, against rust/hadronyche-example: the gain that crosses
// the boundary every way a plug-in can. The expected values are written out by hand, like the other tests here.
using namespace Hadronyche;
using namespace Steinberg;

namespace {

class Processor : public RustProcessor
{
public:
    Processor () : RustProcessor (FUID ()) {}

    // The example shares its count of blocks with sound.
    uint32 blocksShared () const { return shared () ? *static_cast<const uint32*> (shared ()) : 0xFFFFFFFFu; }
};

class Controller : public RustController
{
private:
    IPlugView* createEditor () override { return nullptr; }
};

class TestQueue : public Vst::IParamValueQueue
{
public:
    TestQueue (Vst::ParamID id, std::vector<double> values) : _id (id), _values (std::move (values)) {}

    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }
    Vst::ParamID PLUGIN_API getParameterId () override { return _id; }
    int32 PLUGIN_API getPointCount () override { return static_cast<int32> (_values.size ()); }

    tresult PLUGIN_API getPoint (int32 index, int32& offset, Vst::ParamValue& value) override
    {
        offset = index;
        value = _values[static_cast<size_t> (index)];
        return kResultTrue;
    }

    tresult PLUGIN_API addPoint (int32, Vst::ParamValue, int32&) override { return kResultFalse; }

private:
    Vst::ParamID _id;
    std::vector<double> _values;
};

class TestChanges : public Vst::IParameterChanges
{
public:
    tresult PLUGIN_API queryInterface (const TUID, void**) override { return kNoInterface; }
    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }
    int32 PLUGIN_API getParameterCount () override { return static_cast<int32> (queues.size ()); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData (int32 index) override { return &queues[static_cast<size_t> (index)]; }
    Vst::IParamValueQueue* PLUGIN_API addParameterData (const Vst::ParamID&, int32&) override { return nullptr; }

    std::vector<TestQueue> queues;
};

constexpr int32 kBlockSize = 64;
constexpr double kSampleRate = 44100.0;

// A stereo block through a processor, with the input's two channels at steady levels, and in place when asked.
struct Block
{
    std::array<float, kBlockSize> inputLeft {};
    std::array<float, kBlockSize> inputRight {};
    std::array<float, kBlockSize> outputLeft {};
    std::array<float, kBlockSize> outputRight {};
    uint64 silenceFlags { 0 };

    Block (float left, float right)
    {
        inputLeft.fill (left);
        inputRight.fill (right);
        outputLeft.fill (99.0f);
        outputRight.fill (99.0f);
    }

    void run (Processor& processor, TestChanges* changes = nullptr, bool inPlace = false, int32 numSamples = kBlockSize)
    {
        float* inputs[2] = { inputLeft.data (), inputRight.data () };
        float* outputs[2] = { inPlace ? inputLeft.data () : outputLeft.data (), inPlace ? inputRight.data () : outputRight.data () };
        Vst::AudioBusBuffers inputBus;
        inputBus.numChannels = 2;
        inputBus.channelBuffers32 = inputs;
        Vst::AudioBusBuffers outputBus;
        outputBus.numChannels = 2;
        outputBus.channelBuffers32 = outputs;

        Vst::ProcessData data;
        data.numSamples = numSamples;
        data.symbolicSampleSize = Vst::kSample32;
        data.numInputs = 1;
        data.inputs = &inputBus;
        data.numOutputs = 1;
        data.outputs = &outputBus;
        data.inputParameterChanges = changes;
        processor.process (data);
        silenceFlags = outputBus.silenceFlags;
        if (inPlace)
        {
            outputLeft = inputLeft;
            outputRight = inputRight;
        }
    }

    bool outputIs (float left, float right, float tolerance = 1e-6f) const
    {
        for (int32 i = 0; i < kBlockSize; ++i)
        {
            if (std::abs (outputLeft[static_cast<size_t> (i)] - left) > tolerance || std::abs (outputRight[static_cast<size_t> (i)] - right) > tolerance)
                return false;
        }
        return true;
    }
};

void start (Processor& processor)
{
    processor.initialize (nullptr);
    Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, kBlockSize, kSampleRate };
    processor.setupProcessing (setup);
    processor.setActive (true);
}

std::vector<char> savedState (int32 version, int32 count, const std::vector<std::pair<int32, double>>& pairs)
{
    std::vector<char> bytes;
    const auto add = [&bytes] (const void* data, size_t size) {
        bytes.insert (bytes.end (), static_cast<const char*> (data), static_cast<const char*> (data) + size);
    };
    add (&version, 4);
    add (&count, 4);
    for (const auto& pair : pairs)
    {
        add (&pair.first, 4);
        add (&pair.second, 8);
    }
    return bytes;
}

// Into a stream the caller owns, because MemoryStream copies shallowly, and a copy and its original would free the
// same memory.
void fill (MemoryStream& stream, const std::vector<char>& bytes)
{
    int32 written = 0;
    int64 position = 0;
    if (!bytes.empty ())
        stream.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
    stream.seek (0, IBStream::kIBSeekSet, &position);
}

std::vector<char> bytesOf (MemoryStream& stream)
{
    const auto* data = static_cast<const char*> (stream.getData ());
    return { data, data + stream.getSize () };
}

std::string ascii (const Vst::TChar* string)
{
    char text[128] {};
    UString (const_cast<Vst::TChar*> (string), tstrlen (string)).toAscii (text, sizeof (text));
    return text;
}

// Gain's normalized value for a level in dB, through the same conversion as the processor's defaults, because under
// the SDK's /fp:fast the same formula written out here rounds differently in its last bit.
double gainNormalized (double db)
{
    return plainToNormalized (RustPlugin::getSpec (Example::kGain), db);
}

void testPlugin ()
{
    std::printf ("[plugin]\n");
    const RustPlugin& plugin = RustPlugin::get ();
    check ("the Rust side is there, at this ABI version", plugin.api () != nullptr);
    if (!plugin.api ())
        return;

    check ("it has the example's parameters", plugin.paramCount () == Example::kParamCount && Example::kParamCount == 3);
    check ("its state version", plugin.api ()->state_version == 2);
    check ("it has an audio input", plugin.api ()->has_audio_input);
    check ("and no note input", !plugin.api ()->has_note_input);

    const ParamSpec& gain = plugin.spec (Example::kGain);
    check ("a spec's title", std::strcmp (gain.title, "Gain") == 0);
    check ("its units", std::strcmp (gain.units, "dB") == 0);
    check ("its range", gain.min == -60.0 && gain.max == 6.0 && gain.defaultValue == 0.0);
    check ("its curve and precision", gain.curve == Curve::Linear && gain.precision == 1);
    check ("silent at its minimum", gain.isSilentAtMin && gain.isAutomatable);
    check ("without labels", gain.labels == nullptr && gain.labelCount == 0);

    const ParamSpec& mode = plugin.spec (Example::kMode);
    check ("a list's curve and range", mode.curve == Curve::List && mode.min == 0.0 && mode.max == 1.0);
    check ("its labels", mode.labelCount == 2 && std::strcmp (mode.labels[0], "Stereo") == 0 && std::strcmp (mode.labels[1], "Mono") == 0);
    check ("a list's units are empty", std::strcmp (mode.units, "") == 0);

    const ParamSpec& fail = plugin.spec (Example::kFail);
    check ("a toggle", fail.curve == Curve::Toggle && fail.max == 1.0 && fail.defaultValue == 0.0);
    check ("not automatable when it says so", !fail.isAutomatable && !fail.isSilentAtMin);

    check ("getSpec is the same table", &RustPlugin::getSpec (Example::kMode) == &mode);
}

void testProcessor ()
{
    std::printf ("[processor]\n");
    Processor processor;
    check ("latency follows the sample rate the SDK starts with", processor.getLatencySamples () == 44);
    processor.initialize (nullptr);
    Vst::ProcessSetup setup { Vst::kRealtime, Vst::kSample32, kBlockSize, kSampleRate };
    processor.setupProcessing (setup);
    check ("and known as soon as the host has said the sample rate", processor.getLatencySamples () == 44);
    processor.setActive (true);

    check ("one audio input", processor.getBusCount (Vst::kAudio, Vst::kInput) == 1);
    check ("one audio output", processor.getBusCount (Vst::kAudio, Vst::kOutput) == 1);
    check ("no event input", processor.getBusCount (Vst::kEvent, Vst::kInput) == 0);
    Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
    Vst::SpeakerArrangement mono = Vst::SpeakerArr::kMono;
    check ("stereo in and out is accepted", processor.setBusArrangements (&stereo, 1, &stereo, 1) == kResultOk);
    check ("mono isn't", processor.setBusArrangements (&mono, 1, &mono, 1) == kResultFalse);
    check ("32-bit samples", processor.canProcessSampleSize (Vst::kSample32) == kResultTrue);
    check ("not 64-bit", processor.canProcessSampleSize (Vst::kSample64) == kResultFalse);
    check ("latency comes from the Rust side, prepared at the sample rate", processor.getLatencySamples () == 44);
    check ("and so does the tail", processor.getTailSamples () == Vst::kInfiniteTail);

    {
        Block block (0.5f, -0.25f);
        block.run (processor);
        check ("at the default gain the input comes through", block.outputIs (0.5f, -0.25f));
        check ("and isn't silent", block.silenceFlags == 0);
    }
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kGain, std::vector<double> { 0.0, gainNormalized (6.0) });
        Block block (0.5f, -0.25f);
        block.run (processor, &changes);
        check ("a parameter change reaches the Rust side, by its last point", block.outputIs (0.5f * 1.99526231f, -0.25f * 1.99526231f, 1e-5f));
    }
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kMode, std::vector<double> { 1.0 });
        changes.queues.emplace_back (Example::kGain, std::vector<double> { gainNormalized (0.0) });
        changes.queues.emplace_back (Vst::ParamID (99), std::vector<double> { 1.0 });
        Block block (0.5f, -0.25f);
        block.run (processor, &changes);
        check ("a list parameter arrives as its plain value, and an unknown ID is ignored", block.outputIs (0.125f, 0.125f));
    }
    {
        Block block (0.5f, -0.25f);
        block.run (processor, nullptr, true);
        check ("in place, the output is the same", block.outputIs (0.125f, 0.125f));
    }
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kMode, std::vector<double> { 0.0 });
        Block block (0.0f, 0.0f);
        block.run (processor, &changes);
        check ("silence from the Rust side clears the output", block.outputIs (0.0f, 0.0f));
        check ("and sets both silence flags", block.silenceFlags == 3);
    }
    {
        Block block (0.5f, -0.25f);
        block.run (processor, nullptr, false, kBlockSize);
        check ("sound again after silence", block.outputIs (0.5f, -0.25f) && block.silenceFlags == 0);
    }
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kGain, std::vector<double> { 0.0 });
        Block block (0.5f, -0.25f);
        block.run (processor, &changes);
        check ("the gain's silent minimum is silence", block.outputIs (0.0f, 0.0f) && block.silenceFlags == 3);
    }
    {
        Block block (0.5f, -0.25f);
        Vst::AudioBusBuffers outputBus;
        outputBus.numChannels = 2;
        float* outputs[2] = { block.outputLeft.data (), block.outputRight.data () };
        outputBus.channelBuffers32 = outputs;
        Vst::ProcessData data;
        data.numSamples = 0;
        data.numOutputs = 1;
        data.outputs = &outputBus;
        check ("an empty block is fine", processor.process (data) == kResultOk);
        data.numSamples = kBlockSize;
        data.numOutputs = 0;
        check ("and so is one without outputs", processor.process (data) == kResultOk);
        check ("neither touched the output", block.outputLeft[0] == 99.0f);
    }

    processor.setActive (false);
    processor.terminate ();
}

void testFailure ()
{
    std::printf ("[failure]\n");
    Processor processor;
    start (processor);
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kGain, std::vector<double> { gainNormalized (0.0) });
        changes.queues.emplace_back (Example::kFail, std::vector<double> { 1.0 });
        Block block (0.5f, -0.25f);
        block.run (processor, &changes);
        check ("a panic in the Rust side gives silence", block.outputIs (0.0f, 0.0f) && block.silenceFlags == 3);
    }
    {
        TestChanges changes;
        changes.queues.emplace_back (Example::kFail, std::vector<double> { 0.0 });
        Block block (0.5f, -0.25f);
        block.run (processor, &changes);
        check ("and the instance stays silent afterwards", block.outputIs (0.0f, 0.0f) && block.silenceFlags == 3);
    }
    check ("it still answers the host", processor.getTailSamples () == 0 && processor.getLatencySamples () == 0);
    MemoryStream stream;
    check ("and still saves its state", processor.getState (&stream) == kResultOk && stream.getSize () == 8 + 3 * 12);
    processor.setActive (false);
    processor.terminate ();

    Processor another;
    start (another);
    Block block (0.5f, -0.25f);
    block.run (another);
    check ("a new instance is unaffected", block.outputIs (0.5f, -0.25f) && block.silenceFlags == 0);
    another.setActive (false);
    another.terminate ();
}

void testState ()
{
    std::printf ("[state]\n");
    Processor processor;
    start (processor);

    {
        MemoryStream stream;
        check ("it saves", processor.getState (&stream) == kResultOk);
        auto expected = savedState (2, 3, { { 0, gainNormalized (0.0) }, { 1, 0.0 }, { 2, 0.0 } });
        expected.insert (expected.end (), { 0, 0, 0, 0 });
        check ("the defaults, in the saved format at the Rust side's version, then what it saves after them", bytesOf (stream) == expected);
    }
    {
        check ("the shared pointer is the plug-in's own", processor.blocksShared () == 0);
        Block block (0.5f, -0.25f);
        block.run (processor);
        block.run (processor);
        check ("and follows the instance", processor.blocksShared () == 2);

        MemoryStream saved;
        processor.getState (&saved);
        Processor restored;
        start (restored);
        MemoryStream stream;
        fill (stream, bytesOf (saved));
        check ("another instance restores the state after the parameters", restored.setState (&stream) == kResultOk && restored.blocksShared () == 2);
        restored.setActive (false);
        restored.terminate ();
    }
    {
        MemoryStream stream;
        fill (stream, savedState (1, 2, { { 0, 0.25 }, { 1, 1.0 } }));
        check ("it restores an older version, which ends before what's saved after the parameters", processor.setState (&stream) == kResultOk);
        check ("and that reads as nothing", processor.blocksShared () == 0);
        MemoryStream saved;
        processor.getState (&saved);
        auto expected = savedState (2, 3, { { 0, 0.75 }, { 1, 1.0 }, { 2, 0.0 } });
        expected.insert (expected.end (), { 0, 0, 0, 0 });
        check ("with its values converted by the Rust side", bytesOf (saved) == expected);
        // Gain at 0.75 is -10.5 dB, and Mode at 1 is mono.
        Block block (0.5f, -0.25f);
        block.run (processor);
        check ("and the Rust side gets them", block.outputIs (0.125f * 0.29853826f, 0.125f * 0.29853826f, 1e-5f));
    }
    {
        MemoryStream stream;
        fill (stream, savedState (2, 2, { { 7, 0.5 }, { 1, 0.0 } }));
        check ("an unknown parameter is skipped", processor.setState (&stream) == kResultOk);
        MemoryStream newer;
        fill (newer, savedState (3, 0, {}));
        check ("a newer version is refused", processor.setState (&newer) != kResultOk);
        MemoryStream empty;
        check ("and so is nothing", processor.setState (&empty) != kResultOk);
        check ("and no stream", processor.setState (nullptr) != kResultOk && processor.getState (nullptr) != kResultOk);
    }

    processor.setActive (false);
    processor.terminate ();
}

void testController ()
{
    std::printf ("[controller]\n");
    Controller controller;
    check ("it initializes", controller.initialize (nullptr) == kResultOk);
    check ("with the Rust side's parameters", controller.getParameterCount () == 3);

    Vst::ParameterInfo info {};
    controller.getParameterInfo (Example::kMode, info);
    check ("a list parameter's title", ascii (info.title) == "Mode");
    check ("its steps and flags", info.stepCount == 1 && (info.flags & Vst::ParameterInfo::kIsList) != 0);
    controller.getParameterInfo (Example::kFail, info);
    check ("not automatable when the Rust side says so", (info.flags & Vst::ParameterInfo::kCanAutomate) == 0);

    Vst::String128 text {};
    controller.getParamStringByValue (Example::kGain, 0.0, text);
    check ("a silent minimum shows as -inf", ascii (text) == "-inf");
    controller.getParamStringByValue (Example::kGain, 1.0, text);
    check ("with its precision elsewhere", ascii (text) == "6.0");
    controller.getParamStringByValue (Example::kMode, 1.0, text);
    check ("a list shows its label", ascii (text) == "Mono");
    checkNear ("plain values convert", controller.normalizedParamToPlain (Example::kGain, 0.5), -27.0);

    MemoryStream stream;
    fill (stream, savedState (1, 1, { { 0, 0.25 } }));
    check ("it takes the component's state", controller.setComponentState (&stream) == kResultOk);
    checkNear ("converted the same way as the processor's", controller.getParamNormalized (Example::kGain), 0.75);
    check ("no view without an editor", controller.createView (Vst::ViewType::kEditor) == nullptr);

    controller.terminate ();
}

} // namespace

int main ()
{
    failLoudly ();
    testPlugin ();
    testProcessor ();
    testFailure ();
    testState ();
    testController ();

    std::printf ("\n%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
