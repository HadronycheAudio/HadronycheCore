#include "hadronyche/vst/SpecController.h"
#include "hadronyche/vst/StereoProcessor.h"

// That the headers stand by themselves, and that a plug-in only has to provide what the comments say.
namespace {

constexpr Steinberg::Vst::ParamID kParamCount = 2;

const Hadronyche::ParamSpec& getSpec (Steinberg::Vst::ParamID)
{
    static const Hadronyche::ParamSpec spec { "Gain", "dB", -60.0, 6.0, 0.0, Hadronyche::Curve::Linear, 1 };
    return spec;
}

bool readState (Steinberg::IBStream*, const Hadronyche::ParamValueSink&)
{
    return true;
}

bool writeState (Steinberg::IBStream*, const std::array<double, kParamCount>&)
{
    return true;
}

class MinimalProcessor : public Hadronyche::StereoProcessor<kParamCount>
{
public:
    MinimalProcessor () : StereoProcessor (Steinberg::FUID (), true, getSpec, readState, writeState) {}

private:
    void prepareDsp () override {}
    void renderBlock (Steinberg::Vst::ProcessData&, Steinberg::Vst::AudioBusBuffers& output) override { output.silenceFlags = 0; }
};

class MinimalController : public Hadronyche::SpecController
{
public:
    MinimalController () : SpecController (getSpec, kParamCount, readState) {}

private:
    Steinberg::IPlugView* createEditor () override { return nullptr; }
};

} // namespace

Steinberg::FUnknown* createMinimalProcessor ()
{
    return static_cast<Steinberg::Vst::IAudioProcessor*> (new MinimalProcessor);
}

Steinberg::FUnknown* createMinimalController ()
{
    return static_cast<Steinberg::Vst::IEditController*> (new MinimalController);
}
