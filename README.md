# HadronycheCore

Code shared by the Hadronyche VST® 3 plug-ins: [DiffusionDelay](https://github.com/HadronycheAudio/DiffusionDelay), [LoopTexturer](https://github.com/HadronycheAudio/LoopTexturer), [PartialResynth](https://github.com/HadronycheAudio/PartialResynth) and [RabidPhatt](https://github.com/HadronycheAudio/RabidPhatt).

It isn't a framework, and it only holds what more than one plug-in needs. It's for Windows x64 with Visual Studio 2022, CMake 3.25 or newer and the VST 3 SDK, in C++17.

VST is a registered trademark of Steinberg Media Technologies GmbH.

## What's here

| Target | Headers | Contents |
| --- | --- | --- |
| `hadronyche_dsp` | `hadronyche/dsp/` | `Smoother`, `FlushDenormals`, gain laws, `stepCorrection`, a state-variable filter kernel. No SDK needed |
| `hadronyche_params` | `hadronyche/vst/ParamSpec.h` | Parameter descriptions and their curves. No SDK needed, so that a plug-in's tests can use its parameter IDs |
| `hadronyche_vst` | `hadronyche/vst/` | `SpecParameter`, `ParamStore`, saved parameter state |
| `hadronyche_gui` | `hadronyche/gui/` | `PanelEditor`, a plug-in window built in code from sections, knobs, menus and toggles that follow their parameters |
| `hadronyche_testhost` | `hadronyche/testhost/` | A minimal VST 3 host for test programs, which load the built plug-in and drive it the way a host does, with test signals and measurements. Needs `VST3_SDK_DIR` |

## Using it

A plug-in has this as a submodule at `external/hadronychecore`, and adds it after the SDK, so that it builds with the plug-in's flags and runtime:

```cmake
smtg_enable_vst3_sdk()
add_subdirectory("${HADRONYCHE_CORE_DIR}" "${CMAKE_BINARY_DIR}/hadronychecore")
target_link_libraries(MyPlugin PRIVATE sdk vstgui_support hadronyche_dsp hadronyche_vst hadronyche_gui)
```

A test program links `hadronyche_testhost`, and none of the plug-in's code, so that it can only see what a host can:

```cmake
target_link_libraries(MyPluginTests PRIVATE hadronyche_testhost)
hadronyche_add_host_tests(MyPluginTests MyPlugin first-test second-test)
```

The SDK isn't a submodule here, because a plug-in would then clone it twice.

## Changing it

A change here can reach the saved projects and the sound of every plug-in using it, and nothing may fail at build time. Each plug-in pins its own version, so that a change only arrives when that plug-in's submodule is moved.

To test the library, which needs an SDK checkout for the parts using the SDK:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_DIR=<path to a VST3 SDK>
cmake --build build --config Release --target HadronycheCoreTests HadronycheCoreVstTests HadronycheCoreGuiCheck HadronycheCoreHostCheck
ctest --test-dir build -C Release --output-on-failure
```

The expected values in `tests/` are written out by hand on purpose. Don't change one to make a test pass, because a failure means that saved projects or the sound would change.

Before moving a plug-in to a new version, use `tools/nulltest/` to compare it with the build it replaces. `NullTest` dumps everything a host can observe about a plug-in, and `compare.py` compares two dumps bit for bit.
