# AGENTS.md

Guidance for coding agents working in this repository. Read [Rules that protect users' work](#rules-that-protect-users-work) before changing anything.

HadronycheCore is the code shared by the Hadronyche VST® 3 plug-ins: DiffusionDelay, LoopTexturer, PartialResynth and RabidPhatt. It uses the Steinberg VST3 SDK and VSTGUI directly. JUCE was deliberately rejected, so don't introduce it.

It only holds what more than one plug-in needs, and it isn't a framework. Single-use code stays in its plug-in until a second one needs it. Anything that defines a saved project also stays in the plug-in: parameter IDs and tables, label lists, class IDs and `AppId`.

`README.md` covers using it.

## Commands

The only supported platform is Windows x64 with Visual Studio 2022 and CMake 3.25 or newer.

```powershell
# Configure. The parts using the SDK need a checkout of it, at the same version as the plug-ins.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_DIR=<path to a VST3 SDK>

# Build and run the tests
cmake --build build --config Release --target HadronycheCoreTests HadronycheCoreVstTests HadronycheCoreGuiCheck HadronycheCoreHostCheck
ctest --test-dir build -C Release --output-on-failure

# Work on the library and a plug-in together, from the plug-in's folder
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHADRONYCHE_CORE_DIR=<path to this folder>
```

There's no linter or formatter.

Follow the existing style, which comes from the SDK: a space before call parentheses (`foo (x)`), `_member` for private members, `kName` for constants, and comments that explain why rather than what.

## Rules that protect users' work

People use these plug-ins in real projects. A change here can reach the saved projects and the sound of all of them, and usually nothing fails at build time.

### Saved projects

- **Don't change what `normalizedToPlain` and `plainToNormalized` calculate for an existing `Curve`.** Hosts store values normalized to 0..1, so this silently changes what saved projects sound like. Add a new curve instead, and only at the end of the enum's list if its order ever comes to matter.
- **Don't change the saved format in `ParamState.h`.** Saved projects hold exactly these bytes. A plug-in's own additions go after the parameters, and a plug-in converts old values in the reader it passes in, because the version is per plug-in.
- **Only ever add to the end of `SvfMode`.** Plug-ins cast saved parameter values to it.
- **Don't change the expected values in `tests/` to make a test pass.** They're written out by hand on purpose, and a failure means that saved projects or the sound would change.

### The audio thread

`ParamStore::readChanges` and everything in `hadronyche/dsp/` run on the audio thread, in every plug-in.

- **No allocation, locks, file access, system calls or anything else that can block.** The failure is audio dropouts that may not show up in testing.

### Sound

- **`Smoother` must stay double precision.** In single precision it stalls about 0.01% short of its target, which leaves a held note out of tune after a glide and makes a free running loop the wrong length.
- **`next` and `nextFloat` are not interchangeable.** A single precision signal path relies on `nextFloat` keeping its expressions in `float`. Going through double instead changes the sound, even though it compiles.
- **`stepCorrection`'s reach only works up to a quarter of the sample rate,** which is why oscillators using it fade out by there.
- **The SDK builds with `/fp:fast`.** Restructuring float code can change low-order bits even when the source is equivalent, so compare the output instead of assuming. That's why oscillators share only `stepCorrection`, and not the saw functions around it, which round in a different order.

### Panels

- **Don't change what `PanelLayout`'s defaults or `PanelEditor` draw.** The plug-ins' panel tests click fixed positions, and users see the result. A plug-in's own colours and sizes are passed in.

### Building

- **Keep `SMTG_USE_STATIC_CRT` on, in `cmake/HadronychePlugin.cmake`.** Without it, a plug-in fails to load on PCs without Microsoft's runtime, and neither the tests nor CI would notice, because that runtime is on those machines. Check a change there with `dumpbin /dependents` on a Release DLL from a fresh build folder, because a cached value overrides the default.
- **`hadronyche_use_vst3_sdk` has to stay a macro.** The SDK's flags and targets have to reach the plug-in's own scope.
- **The library is added after the SDK,** so that it builds with the plug-in's flags and runtime. `hadronyche_gui` and `hadronyche_testhost` have their sources compiled as part of whatever links them for the same reason.
- **The SDK isn't a submodule here,** because a plug-in would then clone it twice.
- **The parameter headers mustn't need the SDK.** A plug-in's tests include its parameters without linking it.
- **`LICENSE` ships with every plug-in's installer,** so don't move it.

### The repository

- Don't change git configuration. Commit or push only when asked.
- Follow Steinberg's usage guidelines wherever "VST" appears in product-related material: use ® on first mention, include the sentence "VST is a registered trademark of Steinberg Media Technologies GmbH.", and don't put "VST" in product or company names.

## Architecture

| Target | Needs | Contents |
| --- | --- | --- |
| `hadronyche_dsp` | Nothing | `Smoother`, `FlushDenormals`, gain laws, `stepCorrection`, a state-variable filter kernel |
| `hadronyche_params` | Nothing | `ParamSpec`, its curves and conversions |
| `hadronyche_vst` | SDK | `SpecParameter`, `ParamStore`, saved parameter state |
| `hadronyche_gui` | SDK, VSTGUI | `PanelEditor` |
| `hadronyche_testhost` | SDK headers | A VST3 host for test programs, with signals, measurements and a runner |

- **`PanelEditor`:** a plug-in provides `buildPanel`, and optionally `onSync` and `onClose`. A control's tag is its parameter's ID, and tags from the parameter count upwards are the plug-in's own, which only `valueChanged` handles.
- **The test host:** a plug-in's `Plug` derives from `PlugBase`, and adds what its own features need. Measurements that differ between plug-ins stay in them, such as each `measureFrequency`.

## Verifying a change

Each plug-in pins its own version of this library, so a change only arrives when that plug-in's submodule is moved. Before moving one:

1. **Run the tests here,** in Debug and Release.
2. **Build the plug-in with the new version, in Debug and Release, and run its tests.** Don't weaken or skip them. Its `CompatibilityTests.cpp` guards saved projects.
3. **Compare it with the build it replaces, using `tools/nulltest/`,** unless the change is meant to alter what a host sees, and then check that only that changed:
   - Build `tools/nulltest` once, and keep that binary for both sides of the comparison. It uses only the SDK's interfaces.
   - `tools/nulltest/dump-plugin.sh <NullTest.exe> <tests exe> <.vst3 bundle> <NAME> <folder>` dumps a build: classes, buses, parameters and their text, saved state, how it reads old and damaged state, rendered audio and the panel. `NAME` is the start of the plug-in's `<NAME>_SCREENSHOTS` variable.
   - `tools/nulltest/compare.py <old folder> <new folder>` compares two dumps bit for bit.
   - LoopTexturer's panel test runs against the clock, so add `--panel-ignore 215,185,965,305 --panel-tolerance 700` for it, and look at its panels by eye. Run the old build twice first, to see what varies by itself.
4. **Test it in Cubase** if you can, or say clearly that you couldn't. Nothing here measures how it sounds.
