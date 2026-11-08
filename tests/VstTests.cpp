#include "Check.h"

#include "hadronyche/lv2/Message.h"
#include "hadronyche/vst/ParamSpec.h"
#include "hadronyche/vst/ParamState.h"
#include "hadronyche/vst/SpecParameter.h"

#include "public.sdk/source/common/memorystream.h"

#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace Hadronyche;
using namespace Steinberg;

namespace {

const char* const kLabels[] = { "One", "Two", "Three" };

const ParamSpec kSpecs[] = {
    { "Level", "dB", -60.0, 6.0, 0.0, Curve::Linear, 1, nullptr, 0, true },
    { "Time", "ms", 10.0, 10000.0, 100.0, Curve::Log, 0 },
    { "Choice", "", 0.0, 2.0, 1.0, Curve::List, 0, kLabels, 3 },
    { "Switch", "", 0.0, 1.0, 1.0, Curve::Toggle, 0 },
    { "Bars", "bars", 1.0, 65.0, 33.0, Curve::Integer, 0 },
    { "Mode", "", 0.0, 2.0, 0.0, Curve::List, 0, kLabels, 3, false, false },
};
constexpr int32 kSpecCount = 6;

const ParamSpec& getSpec (Vst::ParamID id)
{
    return kSpecs[id];
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

struct ReadResult
{
    bool ok { false };
    int32 version { -1 };
    std::vector<std::pair<Vst::ParamID, double>> values;
    std::vector<int32> versionsSeen;
};

ReadResult readBytes (const std::vector<char>& bytes, int32 currentVersion = 2)
{
    MemoryStream stream;
    int32 written = 0;
    int64 position = 0;
    if (!bytes.empty ())
        stream.write (const_cast<char*> (bytes.data ()), static_cast<int32> (bytes.size ()), &written);
    stream.seek (0, IBStream::kIBSeekSet, &position);

    ReadResult result;
    result.ok = readParamState (&stream, currentVersion, kSpecCount, [&result] (Vst::ParamID id, double value, int32 version) {
        result.values.push_back ({ id, value });
        result.versionsSeen.push_back (version);
    }, &result.version);
    return result;
}

void testWriting ()
{
    std::printf ("[state-writing]\n");
    const double values[2] = { 0.25, 1.0 };
    MemoryStream stream;
    check ("it writes", writeParamState (&stream, 3, values, 2));

    // The saved format, by hand: version 3, 2 parameters, then ID 0 at 0.25 and ID 1 at 1.0, little-endian.
    const unsigned char expected[] = {
        3, 0, 0, 0, 2, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xD0, 0x3F,
        1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F,
    };
    check ("the bytes are the saved format", stream.getSize () == sizeof (expected) && std::memcmp (stream.getData (), expected, sizeof (expected)) == 0);
}

void testReading ()
{
    std::printf ("[state-reading]\n");
    {
        const auto result = readBytes (savedState (2, 2, { { 0, 0.25 }, { 4, 0.75 } }));
        check ("it reads", result.ok && result.version == 2);
        check ("the values arrive", result.values.size () == 2 && result.values[0].first == 0 && result.values[0].second == 0.25 && result.values[1].first == 4 && result.values[1].second == 0.75);
        check ("with their version", result.versionsSeen.size () == 2 && result.versionsSeen[0] == 2);
    }
    {
        const auto result = readBytes (savedState (1, 1, { { 0, 0.5 } }));
        check ("an older version is read", result.ok && result.versionsSeen[0] == 1 && result.version == 1);
    }
    check ("a newer version is refused", !readBytes (savedState (3, 1, { { 0, 0.5 } })).ok);
    check ("version 0 is refused", !readBytes (savedState (0, 1, { { 0, 0.5 } })).ok);
    check ("a negative version is refused", !readBytes (savedState (-1, 1, { { 0, 0.5 } })).ok);
    check ("nothing at all is refused", !readBytes ({}).ok);

    {
        const auto result = readBytes (savedState (1, 3, { { 99, 0.5 }, { -1, 0.5 }, { 1, 0.5 } }));
        check ("unknown IDs are skipped", result.ok && result.values.size () == 1 && result.values[0].first == 1);
    }
    {
        const double nan = std::numeric_limits<double>::quiet_NaN ();
        const double infinity = std::numeric_limits<double>::infinity ();
        const auto result = readBytes (savedState (1, 3, { { 0, nan }, { 1, infinity }, { 2, 0.5 } }));
        check ("broken values are skipped", result.ok && result.values.size () == 1 && result.values[0].first == 2);
    }
    {
        const auto result = readBytes (savedState (1, 2, { { 0, 2.0 }, { 1, -2.0 } }));
        check ("values are held to the range", result.ok && result.values[0].second == 1.0 && result.values[1].second == 0.0);
    }
    {
        auto bytes = savedState (1, 2, { { 0, 0.5 }, { 1, 0.5 } });
        bytes.resize (bytes.size () - 3);
        const auto result = readBytes (bytes);
        check ("a stream ending early is refused", !result.ok);
        check ("though what came before it was read", result.values.size () == 1);
        check ("and the version isn't reported", result.version == -1);
    }
    {
        const auto result = readBytes (savedState (1, 0, {}));
        check ("no parameters at all is fine", result.ok && result.values.empty ());
    }
}

std::string textOf (const SpecParameter& parameter, double normalized)
{
    Vst::String128 string {};
    parameter.toString (normalized, string);
    char ascii[128] {};
    UString (string, str16BufferSize (Vst::String128)).toAscii (ascii, sizeof (ascii));
    return ascii;
}

bool parse (const SpecParameter& parameter, const char* text, double& value)
{
    Vst::String128 string {};
    UString (string, str16BufferSize (Vst::String128)).fromAscii (text);
    return parameter.fromString (string, value);
}

void testParameterText ()
{
    std::printf ("[parameter-text]\n");

    SpecParameter level (0, kSpecs[0]);
    check ("a silent minimum shows as -inf", textOf (level, 0.0) == "-inf");
    check ("with its precision elsewhere", textOf (level, 1.0) == "6.0");
    double value = -1.0;
    check ("-inf reads back", parse (level, "-inf", value) && value == 0.0);
    check ("a number reads back", parse (level, "6", value) && value == 1.0);
    check ("nonsense is refused", !parse (level, "abc", value));

    SpecParameter time (1, kSpecs[1]);
    check ("no -inf for other parameters", textOf (time, 0.0) == "10");
    value = -1.0;
    check ("and they refuse it", !parse (time, "-inf", value) && value == 0.0);

    SpecParameter choice (2, kSpecs[2]);
    check ("a list shows its label", textOf (choice, 0.5) == "Two");
    check ("and reads it back", parse (choice, "Three", value) && value == 1.0);
    check ("and refuses others", !parse (choice, "Four", value));

    SpecParameter toggle (3, kSpecs[3]);
    check ("a toggle shows On", textOf (toggle, 1.0) == "On");
    check ("and Off", textOf (toggle, 0.0) == "Off");
    check ("reads On", parse (toggle, "On", value) && value == 1.0);
    check ("and anything else as off", parse (toggle, "whatever", value) && value == 0.0);

    SpecParameter bars (4, kSpecs[4]);
    check ("an integer has its text", textOf (bars, 0.5) == "33");

    check ("steps: linear 0", level.getInfo ().stepCount == 0);
    check ("steps: log 0", time.getInfo ().stepCount == 0);
    check ("steps: list labels - 1", choice.getInfo ().stepCount == 2);
    check ("steps: toggle 1", toggle.getInfo ().stepCount == 1);
    check ("steps: integer the range", bars.getInfo ().stepCount == 64);

    check ("flags: automatable", level.getInfo ().flags == Vst::ParameterInfo::kCanAutomate);
    check ("flags: a list is a list", choice.getInfo ().flags == (Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsList));

    SpecParameter mode (5, kSpecs[5]);
    check ("flags: not automatable when it says so", mode.getInfo ().flags == Vst::ParameterInfo::kIsList);

    checkNear ("the default is normalized", level.getInfo ().defaultNormalizedValue, 60.0 / 66.0);
    check ("the id is kept", mode.getInfo ().id == 5);
}

// The LV2 wrapper's messages: what a plug-in sets comes back from the bytes, and bytes from a host that aren't a whole
// message give nothing, without reading past them.
void testMessages ()
{
    using Hadronyche::Lv2::Message;

    auto* message = Message::create ();
    message->setMessageID ("Example");
    message->setInt ("int", -1234567890123);
    message->setFloat ("float", 0.1);
    const Vst::TChar text[] = { 'H', 'i', 0 };
    message->setString ("string", text);
    const unsigned char binary[] = { 0, 1, 2, 255 };
    message->setBinary ("binary", binary, sizeof (binary));
    message->setInt ("replaced", 1);
    message->setFloat ("replaced", 2.0);
    const auto bytes = message->toBytes ();
    message->release ();

    auto* copy = Message::fromBytes (bytes.data (), bytes.size ());
    check ("a message comes back from its bytes", copy != nullptr);
    if (copy)
    {
        int64 integer = 0;
        double number = 0.0;
        Vst::TChar string[8] {};
        const void* data = nullptr;
        uint32 size = 0;
        check ("  with its ID", std::strcmp (copy->getMessageID (), "Example") == 0);
        check ("  an int", copy->getInt ("int", integer) == kResultOk && integer == -1234567890123);
        check ("  a float", copy->getFloat ("float", number) == kResultOk && number == 0.1);
        check ("  a string", copy->getString ("string", string, sizeof (string)) == kResultOk && string[0] == 'H' && string[1] == 'i' && string[2] == 0);
        check ("  binary", copy->getBinary ("binary", data, size) == kResultOk && size == sizeof (binary) && std::memcmp (data, binary, size) == 0);
        check ("  and the last of two values set under one name", copy->getFloat ("replaced", number) == kResultOk && number == 2.0);
        check ("  but not one of another type", copy->getInt ("replaced", integer) != kResultOk && copy->getInt ("missing", integer) != kResultOk);
        check ("  and a string cut to the room given", copy->getString ("string", string, 2 * sizeof (Vst::TChar)) == kResultOk && string[0] == 'H' && string[1] == 0);
        copy->release ();
    }

    // Copied into buffers of exactly their size, so that a read past the end shows in valgrind.
    bool isRefused = true;
    for (size_t size = 0; size < bytes.size (); ++size)
    {
        const std::vector<uint8_t> cut (bytes.begin (), bytes.begin () + static_cast<std::ptrdiff_t> (size));
        auto* partial = Message::fromBytes (cut.data (), cut.size ());
        isRefused = isRefused && partial == nullptr;
        if (partial)
            partial->release ();
    }
    check ("every shortened message is refused", isRefused);

    std::vector<uint8_t> longer = bytes;
    longer.push_back (0);
    check ("one with bytes left over is refused", Message::fromBytes (longer.data (), longer.size ()) == nullptr);

    std::mt19937 random (1);
    int parsed = 0;
    for (int i = 0; i < 20000; ++i)
    {
        std::vector<uint8_t> damaged = bytes;
        for (int j = 0; j < 1 + i % 4; ++j)
            damaged[random () % damaged.size ()] = static_cast<uint8_t> (random ());
        damaged.resize (random () % (damaged.size () + 1));
        if (auto* result = Message::fromBytes (damaged.data (), damaged.size ()))
        {
            ++parsed;
            result->release ();
        }
    }
    std::printf ("  %d of 20000 damaged messages still read as one, and none crashed\n", parsed);
}

} // namespace

int main ()
{
    failLoudly ();
    testWriting ();
    testReading ();
    testParameterText ();
    testMessages ();

    std::printf ("\n%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
