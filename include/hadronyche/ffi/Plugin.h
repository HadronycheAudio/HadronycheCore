// The C side of a plug-in's Rust processor, which RustPlugin.h and RustProcessor.h call through. Generated from
// rust/hadronyche-plugin by cargo test there, so don't edit it here: change the Rust, and run
// HADRONYCHE_UPDATE_HEADERS=1 cargo test in rust/ to bring this up to date.

#pragma once

#include <cstdint>

namespace Hadronyche {
namespace Ffi {

/// Goes up with any change to the structs or functions here. The C++ side refuses a plug-in with another one.
constexpr static const uint32_t ABI_VERSION = 3;

/// What `process` returns: a bit per output channel that holds silence, as a VST3 host's silence flags.
constexpr static const uint32_t SILENT_LEFT = 1;

constexpr static const uint32_t SILENT_RIGHT = 2;

constexpr static const uint32_t SILENT = (SILENT_LEFT | SILENT_RIGHT);

/// What `tail` returns for a tail that never ends by itself, which is VST3's `kInfiniteTail`.
constexpr static const uint32_t INFINITE_TAIL = UINT32_MAX;

constexpr static const uint32_t TRANSPORT_PLAYING = 1;

constexpr static const uint32_t TRANSPORT_TEMPO_VALID = 2;

constexpr static const uint32_t TRANSPORT_PROJECT_TIME_MUSIC_VALID = 4;

constexpr static const uint32_t TRANSPORT_BAR_POSITION_VALID = 8;

constexpr static const uint32_t TRANSPORT_TIME_SIGNATURE_VALID = 16;

/// How a parameter's 0..1 value maps onto its units. The numbers are those of the C++ `Hadronyche::Curve`, which the
/// controller and the LV2 wrapper still convert with, so they're fixed, and a new curve goes at the end.
///
/// Hosts store values normalized to 0..1, so changing the range or curve of an existing parameter silently changes
/// saved projects.
enum class Curve : int32_t {
  Linear = 0,
  Log = 1,
  /// Can reach zero, unlike Log, and still gives short times more of the knob's travel.
  Squared = 2,
  List = 3,
  Toggle = 4,
  Integer = 5,
};

enum class NoteKind : int32_t {
  On = 0,
  Off = 1,
};

/// One parameter, as the C++ side reads it to build its own table. The labels come through `param_label`.
struct ParamInfo {
  const char *title;
  const char *units;
  double min;
  double max;
  double default_value;
  Curve curve;
  int32_t precision;
  uint32_t label_count;
  bool is_silent_at_min;
  bool is_automatable;
};

/// Where the host is, as far as it says. The C++ side fills this from the VST3 process context, and `flags` says
/// which fields it had.
struct Transport {
  uint32_t flags;
  /// Quarter notes per minute.
  double tempo;
  /// The block's start, in quarter notes from the project's start.
  double project_time_music;
  /// The last bar's start, in the same units.
  double bar_position_music;
  int32_t time_signature_numerator;
  int32_t time_signature_denominator;
};

/// A note from the host's event input.
struct NoteEvent {
  NoteKind kind;
  /// Samples into the block.
  int32_t sample_offset;
  /// MIDI pitch, 0 to 127.
  int16_t pitch;
  int16_t channel;
  /// 0 to 1.
  float velocity;
  /// Cents away from the pitch.
  float tuning;
  /// The host's ID for the note, which ties an off to its on, or -1.
  int32_t note_id;
};

/// One block, as the C++ side hands it over. The channel pointers are null for a plug-in without an input, and the
/// output may be the same memory as the input, as a host is allowed to do.
struct ProcessBlock {
  /// One normalized value per parameter, in ID order.
  const double *normalized;
  const float *input_left;
  const float *input_right;
  float *output_left;
  float *output_right;
  uint32_t sample_count;
  Transport transport;
  const NoteEvent *notes;
  uint32_t note_count;
};

/// The C++ side's stream, for `save_state`: writes all of the bytes, or returns false.
using WriteBytes = bool(*)(void *stream, const uint8_t *bytes, uintptr_t count);

/// The C++ side's stream, for `load_state`: fills all of the bytes, or returns false.
using ReadBytes = bool(*)(void *stream, uint8_t *bytes, uintptr_t count);

/// Everything the C++ side calls. An instance is the pointer that `create` returns, which only these functions
/// understand, and every function takes a null one, or a failed one, and does nothing.
struct PluginApi {
  uint32_t abi_version;
  uint32_t param_count;
  int32_t state_version;
  bool has_audio_input;
  bool has_note_input;
  /// Fills `info` for the parameter, and returns false for an ID that's out of range.
  bool (*param_spec)(uint32_t id, ParamInfo *info);
  /// A list parameter's label, or null when there isn't one.
  const char *(*param_label)(uint32_t id, uint32_t index);
  /// A value saved by an older version, as it means now.
  double (*migrate_param)(uint32_t id, double normalized, int32_t version);
  /// A new instance, or null if making one failed.
  void *(*create)();
  void (*destroy)(void *instance);
  /// With one normalized value per parameter, as they stand.
  void (*prepare)(void *instance,
                  double sample_rate,
                  uint32_t max_block_size,
                  const double *normalized);
  uint32_t (*latency)(void *instance);
  uint32_t (*tail)(void *instance);
  /// Runs one block, and returns which output channels hold silence. Audio thread only, and never allocates.
  uint32_t (*process)(void *instance, const ProcessBlock *block);
  /// Writes what the plug-in saves after its parameters, through `write` with `stream`, and returns false if
  /// anything failed. From the host's thread.
  bool (*save_state)(void *instance, WriteBytes write, void *stream);
  /// Reads it back, with the parameters' normalized values as just restored, one per parameter. From the host's
  /// thread.
  bool (*load_state)(void *instance, const double *normalized, ReadBytes read, void *stream);
  /// Something of the plug-in's own, for its C++ side, or null. Valid for as long as the instance is.
  const void *(*shared)(void *instance);
};

}  // namespace Ffi
}  // namespace Hadronyche

// The one symbol a plug-in's Rust side exports, from its export_plugin!.
extern "C" const Hadronyche::Ffi::PluginApi* hadronyche_plugin ();
