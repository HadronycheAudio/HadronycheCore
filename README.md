# HadronycheCore

Code shared by the Hadronyche VST® 3 plug-ins: [DiffusionDelay](https://github.com/HadronycheAudio/DiffusionDelay), [LoopTexturer](https://github.com/HadronycheAudio/LoopTexturer), [PartialResynth](https://github.com/HadronycheAudio/PartialResynth) and [RabidPhatt](https://github.com/HadronycheAudio/RabidPhatt).

It isn't a framework, and it only holds what more than one plug-in needs. It's for Windows x64 with Visual Studio 2022, and macOS on Intel and Apple Silicon with Xcode, using CMake 3.25 or newer and the VST 3 SDK, in C++17.

VST is a registered trademark of Steinberg Media Technologies GmbH.

## What's here

| Target | Headers | Contents |
| --- | --- | --- |
| `hadronyche_dsp` | `hadronyche/dsp/` | `Smoother`, `FlushDenormals`, gain laws, `stepCorrection`, a state-variable filter kernel. No SDK needed |
| `hadronyche_params` | `hadronyche/vst/ParamSpec.h` | Parameter descriptions and their curves. No SDK needed, so that a plug-in's tests can use its parameter IDs |
| `hadronyche_vst` | `hadronyche/vst/` | `SpecParameter`, `ParamStore`, saved parameter state, and the `StereoProcessor` and `SpecController` base classes and `HADRONYCHE_PLUGIN_FACTORY` entry that a plug-in builds on |
| `hadronyche_gui` | `hadronyche/gui/` | `PanelEditor`, a plug-in window built in code from sections, knobs, menus and toggles that follow their parameters |
| `hadronyche_testhost` | `hadronyche/testhost/` | A minimal VST 3 host for test programs, which load the built plug-in and drive it the way a host does, with test signals and measurements, and on Windows `PanelWindow`, which drives a plug-in's real panel off-screen. Needs `VST3_SDK_DIR` |

## Using it

A plug-in has this as a submodule at `external/hadronychecore`, and `cmake/HadronychePlugin.cmake` does the rest. It sets up the SDK, adds this library after it so that it builds with the plug-in's flags and runtime, and links it:

```cmake
set(HADRONYCHE_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/hadronychecore" CACHE PATH "The HadronycheCore library")
include("${HADRONYCHE_CORE_DIR}/cmake/HadronychePlugin.cmake")

hadronyche_use_vst3_sdk()
hadronyche_add_plugin(MyPlugin AU_TYPE aufx AU_SUBTYPE MyPl SOURCES src/PluginEntry.cpp src/Processor.cpp)
```

On Linux, `hadronyche_add_lv2(MyPlugin)` adds an LV2 version built from the same sources, and `hadronyche_add_lv2_tests` compares it with the VST 3.

A test program links `hadronyche_testhost`, and none of the plug-in's code, so that it can only see what a host can:

```cmake
target_link_libraries(MyPluginTests PRIVATE hadronyche_testhost)
hadronyche_add_host_tests(MyPluginTests MyPlugin first-test second-test)
```

A plug-in's CI is `.github/workflows/plugin-ci.yml` here, which its own workflow calls with its name and Audio Unit codes, pinned to the commit its submodule points at. It builds and tests on Windows, macOS and Linux, builds the installers and tarballs with `tools/windows/`, `tools/macos/` and `tools/linux/`, tests them, and publishes a release for a version tag.

The SDK isn't a submodule here, because a plug-in would then clone it twice.

On macOS, this also fetches Apple's AudioUnitSDK, builds an Audio Unit target called `MyPlugin-au` with the SDK's wrapper, and `tools/macos/` builds and tests a `.pkg` installer. A plug-in has to set `CMAKE_OSX_DEPLOYMENT_TARGET` before its `project ()`.

## Changing it

A change here can reach the saved projects and the sound of every plug-in using it, and nothing may fail at build time. Each plug-in pins its own version, so that a change only arrives when that plug-in's submodule is moved. `AGENTS.md` has the rules, and how to verify a change.

To test the library, which needs an SDK checkout for the parts using the SDK:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_DIR=<path to a VST3 SDK>
cmake --build build --config Release --target HadronycheCoreTests HadronycheCoreVstTests HadronycheCoreVstCheck HadronycheCoreGuiCheck HadronycheCoreHostCheck
ctest --test-dir build -C Release --output-on-failure
```

The expected values in `tests/` are written out by hand on purpose. Don't change one to make a test pass, because a failure means that saved projects or the sound would change.

Before moving a plug-in to a new version, use `tools/nulltest/` to compare it with the build it replaces. `NullTest` dumps everything a host can observe about a plug-in, and `compare.py` compares two dumps bit for bit.
