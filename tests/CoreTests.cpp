#include "Check.h"

#include "hadronyche/vst/Denormals.h"
#include "hadronyche/gui/PanelLayout.h"
#include "hadronyche/testhost/Signals.h"
#include "hadronyche/vst/ParamSpec.h"

using namespace Hadronyche;

namespace {

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
    failLoudly ();
    testDenormals ();
    testCurves ();
    testPanelLayout ();
    testSignals ();

    std::printf ("\n%d failure(s)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
