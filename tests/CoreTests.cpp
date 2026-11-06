#include "Check.h"

#include "hadronyche/dsp/BandLimitedStep.h"
#include "hadronyche/dsp/Denormals.h"
#include "hadronyche/dsp/Gain.h"
#include "hadronyche/dsp/Smoother.h"
#include "hadronyche/dsp/Svf.h"
#include "hadronyche/gui/PanelLayout.h"
#include "hadronyche/testhost/Signals.h"
#include "hadronyche/vst/ParamSpec.h"

using namespace Hadronyche;

namespace {

void testSmoother ()
{
    std::printf ("[smoother]\n");
    Smoother smoother;
    smoother.prepare (1000.0, 1.0); // One time constant per step.
    smoother.setTarget (1.0);
    checkNear ("one step covers 1 - 1/e", smoother.next (), 0.632120558828558);
    checkNear ("and the next that of what's left", smoother.next (), 0.864664716763387);

    smoother.snapToTarget ();
    checkNear ("snapToTarget lands on it", smoother.next (), 1.0);

    smoother.snapTo (0.25);
    checkNear ("snapTo moves both", smoother.next (), 0.25);

    // In single precision, this would stall short of the target.
    Smoother slow;
    slow.prepare (48000.0, 100.0);
    slow.setTarget (1.0);
    double value = 0.0;
    for (int i = 0; i < 480000; ++i)
        value = slow.next ();
    checkNear ("it gets all the way to its target", value, 1.0, 1e-12);

    Smoother single;
    single.prepare (1000.0, 1.0);
    single.setTarget (1.0);
    check ("nextFloat is a float", static_cast<double> (single.nextFloat ()) == static_cast<double> (0.632120558828558f));
}

void testGain ()
{
    std::printf ("[gain]\n");
    checkNear ("0 dB", dbToGain (0.0), 1.0);
    checkNear ("-20 dB", dbToGain (-20.0), 0.1);
    checkNear ("6 dB", dbToGain (6.0), 1.99526231496888);
    checkNear ("the silence floor is -60 dB", kVolumeSilenceDb, -60.0);
    checkNear ("Volume at the floor is silent", volumeToGain (-60.0), 0.0);
    checkNear ("Volume below the floor is silent", volumeToGain (-90.0), 0.0);
    checkNear ("Volume just above it isn't", volumeToGain (-59.0), 0.00112201845430196);
    checkNear ("all dry", dryGain (0.0), 1.0);
    checkNear ("dry stays full up to the middle", dryGain (50.0), 1.0);
    checkNear ("and falls after it", dryGain (75.0), 0.5);
    checkNear ("all wet has no dry", dryGain (100.0), 0.0);
    checkNear ("no wet at 0", wetGain (0.0), 0.0);
    checkNear ("wet rises to the middle", wetGain (25.0), 0.5);
    checkNear ("and stays full", wetGain (100.0), 1.0);
}

void testStepCorrection ()
{
    std::printf ("[step-correction]\n");
    checkNear ("nothing 2 samples before", stepCorrection (-2.0), 0.0);
    checkNear ("1 sample before", stepCorrection (-1.0), 1.0 / 24.0);
    checkNear ("half the step just before it", stepCorrection (-1e-15), 0.5, 1e-12);
    checkNear ("and the other half just after", stepCorrection (0.0), -0.5);
    checkNear ("1 sample after", stepCorrection (1.0), -1.0 / 24.0);
    checkNear ("nothing 2 samples after", stepCorrection (2.0), 0.0);
    checkNear ("half a sample before", stepCorrection (-0.5), 0.5 - 0.5 * (2.0 / 3.0 - 0.25 * (1.0 / 3.0 - 0.0625)));
}

void testSvf ()
{
    std::printf ("[svf]\n");
    checkNear ("a frequency is held down to 10 Hz", svfWarp (1.0, 48000.0), std::tan (3.141592653589793 * 10.0 / 48000.0));
    checkNear ("and up to 49% of the sample rate", svfWarp (96000.0, 48000.0), std::tan (3.141592653589793 * 0.49));

    const auto c = svfCoefficients (1.0, 0.5);
    checkNear ("k is 1/Q", c.k, 2.0, 1e-6);
    checkNear ("a1", c.a1, 0.25, 1e-6);
    checkNear ("a2", c.a2, 0.25, 1e-6);
    checkNear ("a3", c.a3, 0.25, 1e-6);

    // One step by hand, from silence, with an input of 1.
    SvfState state;
    const auto taps = svfTick (1.0f, c, state);
    checkNear ("v1", taps.v1, 0.25, 1e-6);
    checkNear ("v2", taps.v2, 0.25, 1e-6);
    checkNear ("ic1", state.ic1, 0.5, 1e-6);
    checkNear ("ic2", state.ic2, 0.5, 1e-6);

    checkNear ("low-pass output", svfOutput (SvfMode::LowPass, 1.0f, 2.0f, { 0.25f, 0.5f }), 0.5, 1e-6);
    checkNear ("high-pass output", svfOutput (SvfMode::HighPass, 1.0f, 2.0f, { 0.25f, 0.25f }), 0.25, 1e-6);
    checkNear ("band-pass output", svfOutput (SvfMode::BandPass, 1.0f, 2.0f, { 0.25f, 0.5f }), 0.5, 1e-6);
    checkNear ("notch output", svfOutput (SvfMode::Notch, 1.0f, 2.0f, { 0.25f, 0.5f }), 0.5, 1e-6);

    // Plug-ins cast saved values to these.
    check ("the modes' values are frozen", static_cast<int> (SvfMode::LowPass) == 0 && static_cast<int> (SvfMode::HighPass) == 1
                                               && static_cast<int> (SvfMode::BandPass) == 2 && static_cast<int> (SvfMode::Notch) == 3);

    SvfState settled;
    const auto lowPass = svfCoefficients (svfWarp (1000.0, 48000.0), 0.707);
    SvfTaps last { 0.0f, 0.0f };
    for (int i = 0; i < 48000; ++i)
        last = svfTick (1.0f, lowPass, settled);
    checkNear ("DC comes through the low-pass", svfOutput (SvfMode::LowPass, 1.0f, lowPass.k, last), 1.0, 1e-3);
    checkNear ("and not the high-pass", svfOutput (SvfMode::HighPass, 1.0f, lowPass.k, last), 0.0, 1e-3);
}

void testDenormals ()
{
    std::printf ("[denormals]\n");
    volatile float tiny = 1e-30f;
    volatile float scale = 1e-10f;
    check ("they exist without it", tiny * scale != 0.0f);
    {
        const FlushDenormals flush;
        check ("and flush to zero with it", tiny * scale == 0.0f);
    }
    check ("and come back afterwards", tiny * scale != 0.0f);
}

void testCurves ()
{
    std::printf ("[curves]\n");
    const char* const labels[] = { "a", "b", "c", "d", "e" };

    // These are what saved projects mean. Changing any of them changes how projects sound.
    const ParamSpec linear { "", "", -60.0, 6.0, 0.0, Curve::Linear, 1 };
    checkNear ("linear to plain", normalizedToPlain (linear, 0.5), -27.0);
    checkNear ("linear to normalized", plainToNormalized (linear, -27.0), 0.5);

    const ParamSpec log { "", "ms", 10.0, 10000.0, 375.0, Curve::Log, 0 };
    checkNear ("log to plain", normalizedToPlain (log, 0.5), 316.227766016838, 1e-9);
    checkNear ("log to normalized", plainToNormalized (log, 100.0), 1.0 / 3.0);

    const ParamSpec squared { "", "s", 0.0, 10.0, 0.0, Curve::Squared, 2 };
    checkNear ("squared to plain", normalizedToPlain (squared, 0.5), 2.5);
    checkNear ("squared to normalized", plainToNormalized (squared, 2.5), 0.5);

    const ParamSpec list { "", "", 0.0, 4.0, 1.0, Curve::List, 0, labels, 5 };
    checkNear ("list rounds down below halfway", normalizedToPlain (list, 0.37), 1.0);
    checkNear ("list rounds up from halfway", normalizedToPlain (list, 0.375), 2.0);
    checkNear ("list to normalized", plainToNormalized (list, 3.0), 0.75);

    const ParamSpec toggle { "", "", 0.0, 1.0, 0.0, Curve::Toggle, 0 };
    checkNear ("toggle below halfway", normalizedToPlain (toggle, 0.49), 0.0);
    checkNear ("toggle from halfway", normalizedToPlain (toggle, 0.5), 1.0);

    const ParamSpec integer { "", "", 1.0, 64.0, 16.0, Curve::Integer, 0 };
    checkNear ("integer rounds", normalizedToPlain (integer, 0.5), 33.0);
    checkNear ("integer to normalized", plainToNormalized (integer, 32.5), 0.5);

    checkNear ("values are held to the range going up", normalizedToPlain (linear, 2.0), 6.0);
    checkNear ("and going down", normalizedToPlain (linear, -1.0), -60.0);
    checkNear ("plain values too, going up", plainToNormalized (linear, 100.0), 1.0);
    checkNear ("and going down", plainToNormalized (linear, -100.0), 0.0);

    check ("automatable unless it says otherwise", linear.isAutomatable);
    check ("not silent unless it says so", !linear.isSilentAtMin);
}

void testPanelLayout ()
{
    std::printf ("[panel-layout]\n");

    // Worked out from the plug-ins' panels as they were before this code was shared. Their panel tests click fixed positions.
    constexpr Hadronyche::PanelLayout layout {};
    checkNear ("a section of 6 columns", layout.sectionWidth (6), 408.0);
    checkNear ("a row of 2 sections", layout.rowWidth ({ 6, 2 }), 2 * 8 + 6 + 408 + 144);
    checkNear ("a row of 1 section", layout.rowWidth ({ 1 }), 2 * 8 + 78);
    checkNear ("the panel's height", layout.rowHeight (), 2 * 8 + 112);
    checkNear ("the knobs' row", layout.knobRow (), 34.0);

    constexpr Hadronyche::PanelLayout taller { 148, 52 };
    checkNear ("a taller section's panel", taller.rowHeight (), 2 * 8 + 148);
    checkNear ("and its knobs' row", taller.knobRow (), 60.0);
    checkNear ("with the other sizes unchanged", taller.columnWidth, 66.0);
}

void testSignals ()
{
    std::printf ("[signals]\n");
    using namespace Hadronyche::TestHost;

    const Samples square { 1.0f, -1.0f, 1.0f, -1.0f };
    checkNear ("rms", rms (square), 1.0);
    checkNear ("rms from a position", rms ({ 0.0f, 0.0f, 0.5f, -0.5f }, 2), 0.5);
    checkNear ("rms of nothing", rms ({}), 0.0);
    checkNear ("peak", peak ({ 0.25f, -0.75f, 0.5f }), 0.75);
    checkNear ("peak from a position", peak ({ 0.25f, -0.75f, 0.5f }, 2), 0.5);

    checkNear ("db of 1", db (1.0), 0.0);
    checkNear ("db of 0.1", db (0.1), -20.0);
    checkNear ("db of nothing has a floor", db (0.0), -240.0);

    check ("slice", slice ({ 1.0f, 2.0f, 3.0f, 4.0f }, 1, 2) == Samples { 2.0f, 3.0f });
    check ("slice stops at the end", slice ({ 1.0f, 2.0f }, 1, 5) == Samples { 2.0f });
    check ("concatenate", concatenate ({ 1.0f }, { 2.0f }) == Samples { 1.0f, 2.0f });
    check ("add", add ({ 1.0f, 2.0f }, { 0.5f }) == Samples { 1.5f, 2.0f });
    check ("finite", isFinite ({ 1.0f, 0.0f }));
    check ("not finite", !isFinite ({ 1.0f, std::nanf ("") }));

    const auto tone = makeTones ({ { 1000.0, 0.5 } }, 48000);
    checkNear ("a tone's rms", rms (tone), 0.5 / std::sqrt (2.0), 1e-6);
    checkNear ("a tone's level", toneLevel (tone, 1000.0), 0.5, 1e-6);
    checkNear ("and away from it", toneLevel (tone, 3000.0), 0.0, 1e-6);
    checkNear ("two tones add", toneLevel (makeTones ({ { 1000.0, 0.5 }, { 2000.0, 0.25 } }, 48000), 2000.0), 0.25, 1e-6);

    // The sequence itself, by hand, since tests have written their limits around it.
    const auto noise = makeNoise (1.0, 2);
    check ("noise is repeatable", noise == makeNoise (1.0, 2));
    checkNear ("noise's first value", noise[0], -0.366812933, 1e-6);
}

} // namespace

int main ()
{
    testSmoother ();
    testGain ();
    testStepCorrection ();
    testSvf ();
    testDenormals ();
    testCurves ();
    testPanelLayout ();
    testSignals ();

    std::printf ("\n%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
