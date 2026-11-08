#include "hadronyche/vst/SpecController.h"

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

class MinimalController : public Hadronyche::SpecController
{
public:
    MinimalController () : SpecController (getSpec, kParamCount, readState) {}

private:
    Steinberg::IPlugView* createEditor () override { return nullptr; }
};

} // namespace

Steinberg::FUnknown* createMinimalController ()
{
    return static_cast<Steinberg::Vst::IEditController*> (new MinimalController);
}
