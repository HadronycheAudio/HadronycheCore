# AGENTS.md

Guidance for coding agents working in this repository. Read [Rules that protect users' work](#rules-that-protect-users-work) before changing anything.

HadronycheCore is the code shared by the Hadronyche VST® 3 plug-ins: DiffusionDelay, LoopTexturer, PartialResynth and RabidPhatt. It uses the Steinberg VST3 SDK and VSTGUI directly. JUCE was deliberately rejected, so don't introduce it.

It holds what more than one plug-in needs, and the frame that a plug-in's Rust processor fits into: `rust/hadronyche-plugin` on the Rust side, and `RustProcessor` and `RustController` on the C++ side, which carry it into the VST3, the Audio Unit and the LV2. Single-use code stays in its plug-in until a second one needs it. Anything that defines a saved project also stays in the plug-in: parameter IDs and tables, label lists, class IDs and `AppId`.

`README.md` covers using it.

## Commands

Windows x64 with Visual Studio 2022 and CMake 3.25 or newer is where this is developed. macOS, on Intel and Apple Silicon with Xcode, is built and tested by CI. Linux builds with Ninja and GCC: `tools/linux/install-build-packages.sh` installs what it needs, and each configuration needs its own folder, with `-G Ninja -DCMAKE_BUILD_TYPE=Release`, because the SDK's Linux output paths assume a single configuration.

```powershell
# Configure. The parts using the SDK need a checkout of it, at the same version as the plug-ins.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_DIR=<path to a VST3 SDK>

# Build and run the tests
cmake --build build --config Release --target HadronycheCoreTests HadronycheCoreVstTests HadronycheCoreRustTests HadronycheCoreVstCheck HadronycheCoreGuiCheck HadronycheCoreHostCheck
ctest --test-dir build -C Release --output-on-failure

# Work on the library and a plug-in together, from the plug-in's folder
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHADRONYCHE_CORE_DIR=<path to this folder>
```

The Rust side in `rust/` has its own checks: `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings` and `cargo test`, run from that folder, which CI runs on all three systems. The parts using the SDK need Rust as well, because `HadronycheCoreRustTests` drives `RustProcessor` against `rust/hadronyche-example`. `cargo test` also checks that `include/hadronyche/ffi/Plugin.h` and `tests/ExampleParamIds.h` are what the Rust side generates, and `HADRONYCHE_UPDATE_HEADERS=1 cargo test` rewrites them.

There's no linter or formatter for the C++.

Follow the existing style, which comes from the SDK: a space before call parentheses (`foo (x)`), `_member` for private members, `kName` for constants, and comments that explain why rather than what.

## Rules that protect users' work

People use these plug-ins in real projects. A change here can reach the saved projects and the sound of all of them, and usually nothing fails at build time.

### Saved projects

- **Don't change what `normalizedToPlain` and `plainToNormalized` calculate for an existing `Curve`.** Hosts store values normalized to 0..1, so this silently changes what saved projects sound like. Add a new curve instead, and only at the end of the enum's list if its order ever comes to matter.
- **Don't change the saved format in `ParamState.h`.** Saved projects hold exactly these bytes. A plug-in's own additions go after the parameters, and a plug-in converts old values in the reader it passes in, because the version is per plug-in. A Rust plug-in's version is its `STATE_VERSION`, and it converts in `migrate_param`, which `RustPlugin::readState` calls for every saved value.
- **A Rust plug-in's parameter IDs are the order of its `params!` enum,** and its generated `ParamIds.h` repeats them for the C++ side. Only ever add at the end, as with a C++ table. The Rust `Curve` has the C++ one's numbers, which `Plugin.h` fixes and `RustPlugin` checks at compile time.
- **Only ever add to the end of `SvfMode`,** in C++ and in Rust. Plug-ins cast saved parameter values to it.
- **Don't change the expected values in `tests/` to make a test pass.** They're written out by hand on purpose, and a failure means that saved projects or the sound would change.

### The audio thread

`ParamStore::readChanges` and everything in `hadronyche/dsp/` run on the audio thread, in every plug-in.

- **No allocation, locks, file access, system calls or anything else that can block.** The failure is audio dropouts that may not show up in testing.
- **Nothing in `rust/hadronyche-dsp` or in a `Processor::process` allocates or reaches the operating system,** and nothing checks that but `lv2compare`'s allocation count: a plug-in sizes everything in `prepare`. `hadronyche-dsp` uses `std` only for the float functions, because those call the platform's maths library as the C++ did, which keeps a port bit for bit with what it replaces. A `no_std` crate would need `libm`, whose `exp`, `tan` and `pow` round differently.
- **`Smoother` in Rust stays `f64`, and `step_f32` is not `step`,** for the reasons the C++ rules give for `Smoother` and `nextFloat`.
- **Every entry into Rust goes through `rust/hadronyche-plugin/src/ffi.rs`, which catches panics,** because unwinding into C++ is undefined. After one, that instance gives silence for good, and the rest of the plug-in goes on, which `HadronycheCoreRustTests` checks. A panic still means a bug.
- **`ffi.rs` copies the input before the processor sees it, and cuts a block longer than `prepare` said into pieces,** because a host may give the same memory as input and output, and Rust's two views of a block must never overlap, and a processor sizes its buffers from `prepare`. Don't hand the host's pointers to a processor directly.

### Sound

- **`Smoother` must stay double precision.** In single precision it stalls about 0.01% short of its target, which leaves a held note out of tune after a glide and makes a free running loop the wrong length.
- **`next` and `nextFloat` are not interchangeable.** A single precision signal path relies on `nextFloat` keeping its expressions in `float`. Going through double instead changes the sound, even though it compiles.
- **`stepCorrection`'s reach only works up to a quarter of the sample rate,** which is why oscillators using it fade out by there.
- **The SDK builds with `/fp:fast`.** Restructuring float code can change low-order bits even when the source is equivalent, so compare the output instead of assuming. That's why oscillators share only `stepCorrection`, and not the saw functions around it, which round in a different order.

### Panels

- **Don't change what `PanelLayout`'s defaults or `PanelEditor` draw.** The plug-ins' panel tests click fixed positions, and users see the result. A plug-in's own colours and sizes are passed in.
- **On Linux, a panel's frame has to open with a run loop in an `X11::FrameConfig`.** Without one, VSTGUI never connects to the X server, and the panel crashes as it opens, in any host. Only `panel-opens` would notice, because the tests and the validator never open a panel otherwise.
- **On Linux, `PanelEditor` has to finish Cairo's device before VSTGUI disconnects from the X server,** which VSTGUI doesn't do itself. Otherwise reopening a panel can abort the host, depending on where the next connection lands in memory.
- **On Linux, plug-ins link with `-z nodelete`, so that closing one never unloads it.** Pango starts a font thread when a panel first draws text, and that thread never ends. A test program that unloaded the plug-in while the thread was busy crashed in the unloaded code, rarely and only on CI, and a host that unloads plug-ins can crash the same way. The test host checks that the plug-in is still loaded after `dlclose`, and `lv2compare` reads the flag from the LV2.

### LV2

- **A plug-in's LV2 URI, port symbols and port numbers are saved in hosts' projects.** `PluginInfo` and `getPortLayout` decide them, so never change `getPortLayout`'s order or what a released plug-in's `PluginInfo` says. A new parameter adds a symbol at the end. Check a generated Turtle against its last release with `lv2info`.
- **The wrapper passes `ModuleEntry` no handle, on purpose.** VSTGUI removes three levels from the binary's path, assuming a `.vst3` bundle, and crashes on a shallow one such as `/opt/Name.lv2/Name.so`. `lv2-shallow-path` and `lv2-panel` guard this, from a bundle directly in `/tmp`. A bundle one folder deeper doesn't crash, and an earlier version of that test passed with the bug put back.
- **`lv2-matches-vst3` compares an LV2's output with its VST3's, bit for bit.** A control port holds a float, so the VST3 side gets the same rounded value. A difference is a bug in the wrapper, so don't loosen it. It also fails if `run ()` allocates, which only works outside valgrind.
- **A state restored into an LV2 keeps its exact values.** It holds doubles, and a port holds a float. A port that hasn't moved, or holds what the state rounds to, isn't a change, because hosts restore ports and state in no fixed order, or not at all. `tools/lv2compare` tests ports left alone, and ports written after the state.
- **The wrapper's binary links with `--no-undefined`.** Without it, a symbol missing from the SDK's libraries only shows when a host reaches that code. `MemoryStream` was one.
- **`run ()` has the audio thread's rules,** and that includes reading atoms. It only uses what it allocated in `create`.
- **A panel's URI, the plug-in's with `#ui`, is saved in hosts' projects too.**
- **Message ports are only for a plug-in with `hasMessagePorts`,** and every addition they make to the Turtle depends on it, so that the other plug-ins' ports and Turtle stay as they were released. Compare their generated Turtle byte for byte after changing the wrapper.
- **The UI never reaches the processor's memory, only its messages,** because a host can run it in another process. A plug-in that shares memory between the two, as LoopTexturer does, sends copies instead from its own `LINKS` sources, as `include/hadronyche/lv2/MessageLink.h` describes.
- **`ui_in` carries `lv2:designation lv2:control`, and the events port doesn't when there are message ports.** REAPER sends a UI's messages only to the port with that mark, and the transport to any port that supports `time:Position`. `lv2-matches-vst3` checks that one port has it.
- **In `run ()`, messages only go through `ByteQueue`s that `create` allocated.** The wrapper's own thread turns them into VST3 messages and delivers them, as a VST3 host's UI thread would, and calls the plug-in's `DspLink`. Anything that doesn't fit is dropped, so a link sends state, not changes.
- **An event in an atom sequence is its time, then the atom with its header, padded to 8 bytes.** `LV2_Atom_Event` already counts that header. Counting it twice passed every check of the sequence's own size, while hosts read every event after the first from the wrong place.
- **Message bytes come from a host, so `Message::fromBytes` and `MessageAtoms::read` trust nothing in them.** `HadronycheCoreVstTests` feeds `fromBytes` every shortened message and damaged ones, and CI runs it under valgrind, because a read past the end doesn't always crash. LoopTexturer's `lv2-looper` sends damaged messages both ways.
- **`lv2compare` counts allocations per thread.** Only the thread calling `run ()` has to keep from allocating, and the message thread and a plug-in's own threads allocate meanwhile.
- **The UI can't require `ui:idleInterface`.** REAPER doesn't offer it, calls idle anyway, and crashes if a UI then refuses to open. Nothing draws without idle calls.
- **The UI's run loop belongs to the process, not to one UI.** VSTGUI keeps the first run loop it gets until its last panel closes, and any UI can close first. Each UI's idle call services it.
- **The run loop calls VSTGUI's event handler on every idle call, without polling its file.** xcb reads ahead, so events can wait in its queue while the file has nothing to read, and the panel then stops drawing.
- **A panel being edited ignores its port.** Hosts echo what the UI writes, late, and the echo would move the knob back under the mouse.
- **`lv2-panel` compares the LV2's panel with the VST3's, pixel for pixel, and what a drag on each writes.** It runs on its own Xvfb screen, wide enough for both. It can't tell whether a panel looks right, so a change to the UI still needs a look in a real host.
- **`jalv` with `jackd1` hangs on any plug-in that reports latency.** It calls `jack_recompute_total_latencies` from its audio thread, which deadlocks with that server. It isn't the plug-in. `jalv` through `pw-jack` is fine.
- **Don't compare REAPER's renders of two formats bit for bit without looking at the tails.** Its VST3 render replaces extremely quiet tails with silence, and its LV2 render doesn't. Rendering at a rate other than the project's adds a resampler as well.

### Building

- **The SDK's `-ffast-math -ffp-contract=fast` stay off on macOS, and fused multiplies stay off on Linux,** through the flags after it in `hadronyche_use_vst3_sdk`. With them, the compiler may remove every `std::isfinite`, and on Apple Silicon `2 * (1 - 100 / 100)` comes out at about -4e-17, so that fully wet effects let through a trace of their dry signal. The plug-ins' tests failed on exactly this.
- **Never change the Audio Unit manufacturer code `Hdrn`.** Hosts use it to find plug-ins in saved projects.
- **Stay on AudioUnitSDK 1.1.0 unless the SDK's wrapper moves on.** It's the first with its headers where the wrapper looks for them, and later ones need newer C++.
- **A plug-in's `CMAKE_OSX_DEPLOYMENT_TARGET` decides which Macs it runs on,** and a build defaults to the machine's own version. `tools/macos/test-installer.sh` reads it from the binaries, because nothing else would notice.
- **Keep `SMTG_USE_STATIC_CRT` on, in `cmake/HadronychePlugin.cmake`.** Without it, a plug-in fails to load on PCs without Microsoft's runtime, and neither the tests nor CI would notice, because that runtime is on those machines. Check a change there with `dumpbin /dependents` on a Release DLL from a fresh build folder, because a cached value overrides the default.
- **`hadronyche_use_vst3_sdk` has to stay a macro.** The SDK's flags and targets have to reach the plug-in's own scope.
- **`hadronyche_add_lv2` builds from the VST3 target's own sources,** so a plug-in calls it after `hadronyche_add_plugin`, and a source added to the VST3 reaches the LV2 by itself.
- **A plug-in's Rust side always builds from the submodule,** because its crate depends on `rust/hadronyche-plugin` and `rust/hadronyche-dsp` by that path, and Cargo can't redirect a path dependency. `hadronyche_add_rust` stops when `HADRONYCHE_CORE_DIR` holds a different `rust/`, so to work on both together, point the plug-in's submodule at the commit here.
- **`include/hadronyche/ffi/Plugin.h` is generated, and `ABI_VERSION` in `ffi.rs` goes up with any change to what's in it.** `cargo test` fails when the header is out of date, and `RustPlugin` treats a Rust side built against another version as a plug-in without parameters, which a plug-in's tests see at once. A plug-in's `ParamIds.h` is generated the same way, by its own `cargo test`.
- **The Rust static library links with the plug-in's own C runtime and GCC runtime.** `hadronyche_add_rust` builds it with `crt-static` to match `SMTG_USE_STATIC_CRT`, and leaves the runtime, and `gcc_s`, out of the libraries that rustc asks for, because the plug-in's link already has them. Check a change there with `dumpbin /dependents` and `ldd`, as for `SMTG_USE_STATIC_CRT`.
- **On macOS, the Rust library is built for both architectures and joined with `lipo`,** because Xcode builds the plug-in universal and Rust has no universal target. `tools/macos/test-installer.sh` checks both slices of the result.
- **A plug-in's `ci.yml` pins a commit of `plugin-ci.yml` here, separately from its submodule.** The jobs run the scripts from the submodule, so the two have to agree: move both together.
- **A Linux release is built in the oldest container that CI uses, not on the runner.** A binary needs a glibc as new as the one it's built against, and only an older distribution would notice. `tools/linux/test-tarball.sh` reads this from the binary, along with the static C++ runtime and the three exported symbols.
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
| `hadronyche_vst` | SDK | `SpecParameter`, `ParamStore`, saved parameter state, `StereoProcessor`, `SpecController`, `HADRONYCHE_PLUGIN_FACTORY` |
| `hadronyche_gui` | SDK, VSTGUI | `PanelEditor` |
| `hadronyche_testhost` | SDK headers | A VST3 host for test programs, with signals, measurements and a runner, on Windows `PanelWindow`, and on Linux `testPanelOpens` |
| `hadronyche_add_lv2` | SDK, VSTGUI, lv2-dev | On Linux, a plug-in's LV2 version: `src/lv2/Lv2Wrapper.cpp` around its VST3 classes, `src/lv2/Ui.cpp` around its panel, VST3 messages between them through `src/lv2/Message.cpp`, with Turtle generated by `src/lv2/TurtleMain.cpp` |
| `hadronyche-dsp` | Rust | `rust/hadronyche-dsp`: `Smoother`, the gain laws, `step_correction`, the state-variable filter kernel and `peak`, the signal processing shared in Rust, which a plug-in's own crate depends on |
| `hadronyche-plugin` | Rust | `rust/hadronyche-plugin`: the `Processor` trait, `params!`, `export_plugin!` and the C ABI in `ffi.rs`, which `include/hadronyche/ffi/Plugin.h` is generated from. `rust/hadronyche-example` is a gain that the tests drive through it |
| `hadronyche_add_rust` | Rust, Cargo | `cmake/HadronycheRust.cmake` builds a plug-in's crate into its VST3, LV2 and Turtle generator, as `RustPlugin.h`, `RustProcessor.h` and `RustController.h` in `hadronyche_vst` expect |

`.github/workflows/plugin-ci.yml` is every plug-in's CI, which each calls with its name and Audio Unit codes. `tools/windows/` tests a plug-in's Windows installer on CI, `tools/macos/` builds its `.pkg` installer and tests it, `tools/linux/` does the same for its tarballs, `tools/nulltest/` compares two builds, on Windows and Linux, `tools/lv2compare/` compares a plug-in's LV2 with its VST3, and `tools/lv2panel/` compares their panels.

- **The LV2 wrapper:** a plug-in provides `src/lv2/PluginInfo.cpp`, and calls `hadronyche_add_lv2` and `hadronyche_add_lv2_tests`. It has audio, MIDI, control ports, state, latency, transport and the plug-in's own panel. With `hasMessagePorts`, it also carries the VST3 messages between the processor and the panel, through the ports `ui_in` and `ui_out`, and the plug-in can give `hadronyche_add_lv2` `LINKS` sources that change what passes, as LoopTexturer's `src/lv2/LooperLink.cpp` does.

- **`RustProcessor`, `RustController` and `RustPlugin`:** a plug-in with its processing in Rust derives its processor and controller from the first two, adding only its class IDs and its editor. `RustPlugin` reaches the Rust side through `hadronyche_plugin ()`, the one symbol that `export_plugin!` exports, and gives its parameter table as `ParamSpec`s, so `RustPlugin::getSpec` goes wherever a C++ plug-in's `getParamSpec` went: the panel and `PluginInfo`. The processor keeps the parameters' normalized values, as `ParamStore` does, and hands each block a snapshot of them, the audio, the transport and the notes. Everything a host sees, from buses to state bytes, is decided on the C++ side, the same way as in `StereoProcessor`.

- **`StereoProcessor` and `SpecController`:** a plug-in with its processing in C++ derives its processor and controller from these, with their class IDs and `Parameters.h`. The processor provides `prepareDsp` and `renderBlock`, which gets each stereo block after the parameter changes are taken and denormals are flushed, and overrides `initialize`, `setState` or `getState` for an event input or more state, calling `restoreParams` and `saveParams`. `PluginEntry.cpp` is one `HADRONYCHE_PLUGIN_FACTORY` line.

- **`PanelEditor`:** a plug-in provides `buildPanel`, and optionally `onSync` and `onClose`. A control's tag is its parameter's ID, and tags from the parameter count upwards are the plug-in's own, which only `valueChanged` handles.
- **The test host:** a plug-in's `Plug` derives from `PlugBase`, and adds what its own features need. `paramText`, `paramFromText`, `readAt`, `nearly` and `setStateBytes` are what every plug-in's compatibility and state tests use. Measurements that differ between plug-ins stay in them, such as each `measureFrequency`.

## Verifying a change

Each plug-in pins its own version of this library, so a change only arrives when that plug-in's submodule is moved, together with the commit in its `.github/workflows/ci.yml`. Before moving one:

1. **Run the tests here,** in Debug and Release.
2. **Build the plug-in with the new version, in Debug and Release, and run its tests.** Don't weaken or skip them. Its `CompatibilityTests.cpp` guards saved projects.
3. **Compare it with the build it replaces, using `tools/nulltest/`,** unless the change is meant to alter what a host sees, and then check that only that changed:
   - Build `tools/nulltest` once, and keep that binary for both sides of the comparison. It uses only the SDK's interfaces.
   - `tools/nulltest/dump-plugin.sh <NullTest> <tests program> <.vst3 bundle> <NAME> <folder>` dumps a build: classes, buses, parameters and their text, saved state, how it reads old and damaged state, rendered audio and, on Windows only, the panel. `NAME` is the start of the plug-in's `<NAME>_SCREENSHOTS` variable.
   - Output differs between platforms, so only compare builds from the same platform.
   - `tools/nulltest/compare.py <old folder> <new folder>` compares two dumps bit for bit. `--audio-tolerance -120` lets audio through whose peak difference is at or below that level, for a change that can't be bit for bit, such as a port to Rust whose maths library rounds differently, and still says how far off it is.
   - LoopTexturer's panel test runs against the clock, so add `--panel-ignore 215,185,965,305 --panel-tolerance 700` for it, and look at its panels by eye. Run the old build twice first, to see what varies by itself.
4. **Test it in Cubase** if you can, or say clearly that you couldn't. Nothing here measures how it sounds.
