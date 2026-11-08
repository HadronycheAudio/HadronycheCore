//! What a plug-in's signal processing implements, and what each block brings it.

use crate::{ParamSpec, Params};

/// A plug-in's signal processing. The C++ side creates one of these per instance, prepares it when the host
/// activates the plug-in, and calls `process` for each block from the audio thread.
///
/// `process` runs with the audio thread's rules: nothing in it may allocate, lock, or reach the operating system.
/// `new` and `prepare` may allocate, and `prepare` is where buffers get their size.
pub trait Processor: Send + 'static {
    /// The parameter table, in ID order, from [`params!`](crate::params).
    const PARAMS: &'static [ParamSpec];

    /// The version written into saved state. Raise it when a saved value has to be read differently, and convert
    /// the old ones in [`migrate_param`](Self::migrate_param).
    const STATE_VERSION: i32 = 1;

    /// Whether the plug-in has a stereo input, as well as its stereo output.
    const HAS_AUDIO_INPUT: bool;

    /// Whether the plug-in takes notes, which gives it an event input.
    const HAS_NOTE_INPUT: bool = false;

    fn new() -> Self;

    /// Before the first block, and again each time the host activates the plug-in. Blocks are never longer than
    /// `max_block_size`.
    fn prepare(&mut self, sample_rate: f64, max_block_size: usize);

    /// One block. Its output starts with whatever the host left there.
    fn process(&mut self, block: Block<'_>) -> Output;

    /// How many samples late the output is, which the host compensates for.
    fn latency(&self) -> u32 {
        0
    }

    /// How long the output goes on after the input stops.
    fn tail(&self) -> Tail {
        Tail::Samples(0)
    }

    /// A parameter's value from a project saved with an older `STATE_VERSION`, as it means now. Called for every
    /// saved value, with the version it was saved with.
    fn migrate_param(index: usize, normalized: f64, version: i32) -> f64 {
        let _ = (index, version);
        normalized
    }

    /// Whatever the plug-in saves after its parameters, as ParamState.h's layout has room for. From the host's
    /// thread, not the audio thread, and it may allocate. Nothing by default.
    fn save_state(&self, out: &mut dyn StateWriter) -> bool {
        let _ = out;
        true
    }

    /// Reads back what `save_state` wrote, after the parameters have been restored and with their values, which an
    /// older project's state may not have. The stream ends where the parameters ended for a state saved before the
    /// plug-in saved anything more, and that has to read as nothing. From the host's thread, and it may allocate.
    fn load_state(&mut self, params: &Params<'_>, input: &mut dyn StateReader) -> bool {
        let _ = (params, input);
        true
    }

    /// Something of the plug-in's own that its C++ side reaches through `RustProcessor::shared ()`, such as what its
    /// panel reads, for as long as the instance lives. Null by default.
    fn shared(&self) -> *const core::ffi::c_void {
        core::ptr::null()
    }
}

/// Where `save_state` writes: the host's stream, through the C++ side.
pub trait StateWriter {
    /// All of `bytes`, or false.
    fn write(&mut self, bytes: &[u8]) -> bool;
}

/// Where `load_state` reads: the host's stream, through the C++ side.
pub trait StateReader {
    /// Fills all of `bytes`, or returns false, which a stream that has ended does.
    fn read(&mut self, bytes: &mut [u8]) -> bool;
}

/// What a processor returns for a block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Output {
    /// The output holds sound.
    Sound,
    /// Nothing to hear: the output is cleared, and the host is told that it can skip it.
    Silence,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tail {
    Samples(u32),
    /// Never ends by itself, as with feedback that can reach 100%. The silence of each block says when it has.
    Infinite,
}

/// One block of audio, with everything the host sent along with it.
pub struct Block<'a> {
    /// The host's parameter values, as they stand at this block. Smoothing is the processor's job.
    pub params: Params<'a>,
    /// Left and right, or `None` for a plug-in without an input. Never the same memory as the output.
    pub input: Option<[&'a [f32]; 2]>,
    /// Left and right, each as long as the block.
    pub output: [&'a mut [f32]; 2],
    pub transport: &'a Transport,
    /// In the order the host gave them, each with its offset into the block.
    pub notes: &'a [NoteEvent],
}

impl Block<'_> {
    pub fn len(&self) -> usize {
        self.output[0].len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Where the host is, as far as it says. The C++ side fills this from the VST3 process context, and `flags` says
/// which fields it had.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Transport {
    pub flags: u32,
    /// Quarter notes per minute.
    pub tempo: f64,
    /// The block's start, in quarter notes from the project's start.
    pub project_time_music: f64,
    /// The last bar's start, in the same units.
    pub bar_position_music: f64,
    pub time_signature_numerator: i32,
    pub time_signature_denominator: i32,
}

pub const TRANSPORT_PLAYING: u32 = 1;
pub const TRANSPORT_TEMPO_VALID: u32 = 2;
pub const TRANSPORT_PROJECT_TIME_MUSIC_VALID: u32 = 4;
pub const TRANSPORT_BAR_POSITION_VALID: u32 = 8;
pub const TRANSPORT_TIME_SIGNATURE_VALID: u32 = 16;

impl Transport {
    pub fn is_playing(&self) -> bool {
        self.flags & TRANSPORT_PLAYING != 0
    }

    /// The tempo, if the host gave a usable one. A plug-in keeps the last one for hosts that don't always.
    pub fn tempo(&self) -> Option<f64> {
        (self.flags & TRANSPORT_TEMPO_VALID != 0 && self.tempo.is_finite() && self.tempo > 0.0).then_some(self.tempo)
    }

    pub fn project_time_music(&self) -> Option<f64> {
        (self.flags & TRANSPORT_PROJECT_TIME_MUSIC_VALID != 0).then_some(self.project_time_music)
    }

    pub fn bar_position_music(&self) -> Option<f64> {
        (self.flags & TRANSPORT_BAR_POSITION_VALID != 0).then_some(self.bar_position_music)
    }

    pub fn time_signature(&self) -> Option<(i32, i32)> {
        (self.flags & TRANSPORT_TIME_SIGNATURE_VALID != 0)
            .then_some((self.time_signature_numerator, self.time_signature_denominator))
    }
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NoteKind {
    On = 0,
    Off = 1,
}

/// A note from the host's event input.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct NoteEvent {
    pub kind: NoteKind,
    /// Samples into the block.
    pub sample_offset: i32,
    /// MIDI pitch, 0 to 127.
    pub pitch: i16,
    pub channel: i16,
    /// 0 to 1.
    pub velocity: f32,
    /// Cents away from the pitch.
    pub tuning: f32,
    /// The host's ID for the note, which ties an off to its on, or -1.
    pub note_id: i32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transport_only_gives_what_the_host_marked() {
        let empty = Transport::default();
        assert!(!empty.is_playing());
        assert_eq!(empty.tempo(), None);
        assert_eq!(empty.project_time_music(), None);
        assert_eq!(empty.time_signature(), None);

        let full = Transport {
            flags: TRANSPORT_PLAYING
                | TRANSPORT_TEMPO_VALID
                | TRANSPORT_PROJECT_TIME_MUSIC_VALID
                | TRANSPORT_BAR_POSITION_VALID
                | TRANSPORT_TIME_SIGNATURE_VALID,
            tempo: 120.0,
            project_time_music: 9.5,
            bar_position_music: 8.0,
            time_signature_numerator: 3,
            time_signature_denominator: 4,
        };
        assert!(full.is_playing());
        assert_eq!(full.tempo(), Some(120.0));
        assert_eq!(full.project_time_music(), Some(9.5));
        assert_eq!(full.bar_position_music(), Some(8.0));
        assert_eq!(full.time_signature(), Some((3, 4)));
    }

    #[test]
    fn a_tempo_has_to_be_usable() {
        let zero = Transport {
            flags: TRANSPORT_TEMPO_VALID,
            tempo: 0.0,
            ..Transport::default()
        };
        assert_eq!(zero.tempo(), None);
        let nan = Transport {
            flags: TRANSPORT_TEMPO_VALID,
            tempo: f64::NAN,
            ..Transport::default()
        };
        assert_eq!(nan.tempo(), None);
    }
}
