#pragma once

// A minimal VST3 host. It loads the built plug-in and drives it through the same interfaces a real host
// uses, so the tests exercise the real binary rather than individual classes.

#include "hadronyche/testhost/Signals.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Hadronyche {
namespace TestHost {

constexpr int kBlockSize = 256;

struct Stereo
{
    Samples left;
    Samples right;
    std::vector<bool> blockSilent; // The silence flags the plug-in set, per block.
};

inline void sleepMilliseconds (int milliseconds)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (milliseconds));
}

// Loading the plug-in.
bool loadPlugin (const wchar_t* path);
void unloadPlugin ();
Steinberg::IPluginFactory* getFactory ();

// Results. Measurements print as they go, so that a failure comes with its numbers.
void check (const char* what, bool ok);
int getFailureCount ();

// Equal to within a millionth, for values that have been through a controller's conversions.
inline bool nearly (double a, double b)
{
    return std::abs (a - b) < 1e-6 * std::max (1.0, std::abs (b));
}

// A value out of saved state bytes, for checking the format.
template <typename T>
T readAt (const std::vector<char>& bytes, size_t offset)
{
    T value {};
    std::memcpy (&value, bytes.data () + offset, sizeof (value));
    return value;
}

// What the controller shows for a normalized value.
std::string paramText (Steinberg::Vst::IEditController* controller, Steinberg::Vst::ParamID id, double normalized);

// Whether the controller understands text for a parameter, and the normalized value it means.
bool paramFromText (Steinberg::Vst::IEditController* controller, Steinberg::Vst::ParamID id, const char* text, double& normalized);

// One instance of the plug-in, connected and active, as a host would set it up. A plug-in's tests add to this
// whatever its own features need.
class PlugBase
{
public:
    explicit PlugBase (double sampleRate = kSampleRate, int maxBlockSize = kBlockSize);
    ~PlugBase ();

    PlugBase (const PlugBase&) = delete;
    PlugBase& operator= (const PlugBase&) = delete;

    // Applied to both sides, as a host would forward it, and takes effect in the next block.
    void set (Steinberg::Vst::ParamID id, double plain);

    // Runs the input through in blocks, on both channels unless it's given one for each.
    Stereo process (const Samples& mono, int blockSize = kBlockSize);
    Stereo process (const Samples& left, const Samples& right, int blockSize = kBlockSize);

    std::vector<char> getStateBytes ();

    // Loads a state into both sides, as a host does when it opens a project, and says whether both took it.
    bool setStateBytes (const std::vector<char>& bytes);

    // What the host says the tempo is from now on. A plug-in starts with no tempo from the host at all, and zero
    // goes back to that. It replaces any other context.
    void setTempo (double bpm);

    Steinberg::Vst::IComponent* component { nullptr };
    Steinberg::Vst::IAudioProcessor* processor { nullptr };
    Steinberg::Vst::IEditController* controller { nullptr };
    Steinberg::Vst::ProcessContext* context { nullptr }; // Null means the host gives no transport or tempo.

protected:
    // One block of stereo, with any parameter changes that are waiting, and returns the output's silence flags. With
    // no inputs, the plug-in gets no input bus.
    Steinberg::uint64 runBlock (float** inputs, float** outputs, int count, Steinberg::Vst::IEventList* events = nullptr);

    // Sends the processor a message with one integer attribute, the way a plug-in's controller would.
    void sendToProcessor (const char* messageId, const char* attribute, Steinberg::int64 value);

    double _sampleRate;

private:
    class ParameterChanges;

    std::unique_ptr<ParameterChanges> _changes;
    Steinberg::Vst::ProcessContext _tempoContext {};
    Steinberg::Vst::IConnectionPoint* _componentConnection { nullptr };
    Steinberg::Vst::IConnectionPoint* _controllerConnection { nullptr };
};

} // namespace TestHost
} // namespace Hadronyche
