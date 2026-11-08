//! The C side of a plug-in: a table of functions that the C++ `RustProcessor` and `RustPlugin` call, reached through
//! the symbol that [`export_plugin!`](crate::export_plugin) defines. `include/hadronyche/ffi/Plugin.h` is generated
//! from this file, so a change here is a change to that header, and `ABI_VERSION` goes up when the two could be
//! mixed.
//!
//! Every function here catches a panic, because unwinding into C++ is undefined. After one, the instance is marked
//! as failed and gives silence, and the rest of the plug-in goes on.

use core::ffi::{c_char, c_void};
use core::ptr;
use core::slice;
use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::{Curve, NoteEvent, Output, Params, Processor, StateReader, StateWriter, Tail, Transport};

/// Goes up with any change to the structs or functions here. The C++ side refuses a plug-in with another one.
pub const ABI_VERSION: u32 = 3;

/// The C++ side's stream, for `save_state`: writes all of the bytes, or returns false.
pub type WriteBytes = unsafe extern "C" fn(stream: *mut c_void, bytes: *const u8, count: usize) -> bool;
/// The C++ side's stream, for `load_state`: fills all of the bytes, or returns false.
pub type ReadBytes = unsafe extern "C" fn(stream: *mut c_void, bytes: *mut u8, count: usize) -> bool;

/// What `process` returns: a bit per output channel that holds silence, as a VST3 host's silence flags.
pub const SILENT_LEFT: u32 = 1;
pub const SILENT_RIGHT: u32 = 2;
pub const SILENT: u32 = SILENT_LEFT | SILENT_RIGHT;

/// What `tail` returns for a tail that never ends by itself, which is VST3's `kInfiniteTail`.
pub const INFINITE_TAIL: u32 = u32::MAX;

/// One parameter, as the C++ side reads it to build its own table. The labels come through `param_label`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ParamInfo {
    pub title: *const c_char,
    pub units: *const c_char,
    pub min: f64,
    pub max: f64,
    pub default_value: f64,
    pub curve: Curve,
    pub precision: i32,
    pub label_count: u32,
    pub is_silent_at_min: bool,
    pub is_automatable: bool,
}

/// One block, as the C++ side hands it over. The channel pointers are null for a plug-in without an input, and the
/// output may be the same memory as the input, as a host is allowed to do.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ProcessBlock {
    /// One normalized value per parameter, in ID order.
    pub normalized: *const f64,
    pub input_left: *const f32,
    pub input_right: *const f32,
    pub output_left: *mut f32,
    pub output_right: *mut f32,
    pub sample_count: u32,
    pub transport: Transport,
    pub notes: *const NoteEvent,
    pub note_count: u32,
}

/// Everything the C++ side calls. An instance is the pointer that `create` returns, which only these functions
/// understand, and every function takes a null one, or a failed one, and does nothing.
#[repr(C)]
pub struct PluginApi {
    pub abi_version: u32,
    pub param_count: u32,
    pub state_version: i32,
    pub has_audio_input: bool,
    pub has_note_input: bool,

    /// Fills `info` for the parameter, and returns false for an ID that's out of range.
    pub param_spec: unsafe extern "C" fn(id: u32, info: *mut ParamInfo) -> bool,
    /// A list parameter's label, or null when there isn't one.
    pub param_label: unsafe extern "C" fn(id: u32, index: u32) -> *const c_char,
    /// A value saved by an older version, as it means now.
    pub migrate_param: unsafe extern "C" fn(id: u32, normalized: f64, version: i32) -> f64,

    /// A new instance, or null if making one failed.
    pub create: unsafe extern "C" fn() -> *mut c_void,
    pub destroy: unsafe extern "C" fn(instance: *mut c_void),
    /// With one normalized value per parameter, as they stand.
    pub prepare:
        unsafe extern "C" fn(instance: *mut c_void, sample_rate: f64, max_block_size: u32, normalized: *const f64),
    pub latency: unsafe extern "C" fn(instance: *mut c_void) -> u32,
    pub tail: unsafe extern "C" fn(instance: *mut c_void) -> u32,
    /// Runs one block, and returns which output channels hold silence. Audio thread only, and never allocates.
    pub process: unsafe extern "C" fn(instance: *mut c_void, block: *const ProcessBlock) -> u32,

    /// Writes what the plug-in saves after its parameters, through `write` with `stream`, and returns false if
    /// anything failed. From the host's thread.
    pub save_state: unsafe extern "C" fn(instance: *mut c_void, write: WriteBytes, stream: *mut c_void) -> bool,
    /// Reads it back, with the parameters' normalized values as just restored, one per parameter. From the host's
    /// thread.
    pub load_state: unsafe extern "C" fn(
        instance: *mut c_void,
        normalized: *const f64,
        read: ReadBytes,
        stream: *mut c_void,
    ) -> bool,
    /// Something of the plug-in's own, for its C++ side, or null. Valid for as long as the instance is.
    pub shared: unsafe extern "C" fn(instance: *mut c_void) -> *const c_void,
}

impl PluginApi {
    /// The table for a processor type, which [`export_plugin!`](crate::export_plugin) puts in a static.
    pub const fn of<P: Processor>() -> Self {
        Self {
            abi_version: ABI_VERSION,
            param_count: P::PARAMS.len() as u32,
            state_version: P::STATE_VERSION,
            has_audio_input: P::HAS_AUDIO_INPUT,
            has_note_input: P::HAS_NOTE_INPUT,
            param_spec: param_spec::<P>,
            param_label: param_label::<P>,
            migrate_param: migrate_param::<P>,
            create: create::<P>,
            destroy: destroy::<P>,
            prepare: prepare::<P>,
            latency: latency::<P>,
            tail: tail::<P>,
            process: process::<P>,
            save_state: save_state::<P>,
            load_state: load_state::<P>,
            shared: shared::<P>,
        }
    }
}

// SAFETY: it holds only plain values and function pointers.
unsafe impl Sync for PluginApi {}

/// What's behind an instance pointer.
struct Instance<P> {
    processor: P,
    /// Set by a panic, after which every call gives silence.
    failed: bool,
    /// After `prepare`. A block before it is silence, because the processor has no sample rate yet.
    max_block_size: usize,
    /// Copies of the input, for a host that gives the same memory as the output: the processor sees them as two.
    input_copy: [Vec<f32>; 2],
    /// The notes of one piece of a block that had to be cut, with their offsets moved to it.
    note_copy: Vec<NoteEvent>,
}

/// How many notes a piece of a cut block can carry. More in one block than this is no host's doing.
const MAX_NOTES: usize = 1024;

/// The notes that fall into the piece of a block that starts at `start`, with offsets into the piece, written into
/// `copy`. The notes before the block's start go with its first piece, and those past its end with its last.
fn notes_of_piece<'a>(
    copy: &'a mut Vec<NoteEvent>,
    notes: &[NoteEvent],
    start: usize,
    len: usize,
    is_last_piece: bool,
) -> &'a [NoteEvent] {
    let first = start as i32;
    let last = (start + len) as i32;
    copy.clear();
    for note in notes {
        let belongs = (note.sample_offset >= first || start == 0) && (note.sample_offset < last || is_last_piece);
        if belongs && copy.len() < MAX_NOTES {
            copy.push(NoteEvent {
                sample_offset: note.sample_offset - first,
                ..*note
            });
        }
    }
    copy
}

unsafe extern "C" fn param_spec<P: Processor>(id: u32, info: *mut ParamInfo) -> bool {
    let Some(spec) = P::PARAMS.get(id as usize) else {
        return false;
    };
    if info.is_null() {
        return false;
    }
    // SAFETY: the caller gives a struct to fill.
    unsafe {
        info.write(ParamInfo {
            title: spec.title.as_ptr(),
            units: spec.units.as_ptr(),
            min: spec.min,
            max: spec.max,
            default_value: spec.default,
            curve: spec.curve,
            precision: spec.precision,
            label_count: spec.labels.len() as u32,
            is_silent_at_min: spec.is_silent_at_min,
            is_automatable: spec.is_automatable,
        });
    }
    true
}

unsafe extern "C" fn param_label<P: Processor>(id: u32, index: u32) -> *const c_char {
    P::PARAMS
        .get(id as usize)
        .and_then(|spec| spec.labels.get(index as usize))
        .map_or(ptr::null(), |label| label.as_ptr())
}

unsafe extern "C" fn migrate_param<P: Processor>(id: u32, normalized: f64, version: i32) -> f64 {
    catch_unwind(|| P::migrate_param(id as usize, normalized, version)).unwrap_or(normalized)
}

unsafe extern "C" fn create<P: Processor>() -> *mut c_void {
    match catch_unwind(|| {
        Box::new(Instance {
            processor: P::new(),
            failed: false,
            max_block_size: 0,
            input_copy: [Vec::new(), Vec::new()],
            note_copy: Vec::new(),
        })
    }) {
        Ok(instance) => Box::into_raw(instance).cast(),
        Err(_) => ptr::null_mut(),
    }
}

unsafe extern "C" fn destroy<P: Processor>(instance: *mut c_void) {
    if instance.is_null() {
        return;
    }
    // SAFETY: the pointer came from create, and the caller gives it up here.
    let instance = unsafe { Box::from_raw(instance.cast::<Instance<P>>()) };
    // A panic in a destructor would otherwise unwind into C++, and there's nothing to do about it here.
    let _ = catch_unwind(AssertUnwindSafe(move || drop(instance)));
}

/// Runs `call` on a live instance, and gives `fallback` for a null or failed one, or when `call` panics, which fails
/// the instance.
unsafe fn with<P: Processor, R>(instance: *mut c_void, fallback: R, call: impl FnOnce(&mut Instance<P>) -> R) -> R {
    if instance.is_null() {
        return fallback;
    }
    // SAFETY: the pointer came from create, and only one thread at a time calls into an instance.
    let instance = unsafe { &mut *instance.cast::<Instance<P>>() };
    if instance.failed {
        return fallback;
    }
    match catch_unwind(AssertUnwindSafe(|| call(instance))) {
        Ok(result) => result,
        Err(_) => {
            instance.failed = true;
            fallback
        }
    }
}

unsafe extern "C" fn prepare<P: Processor>(
    instance: *mut c_void,
    sample_rate: f64,
    max_block_size: u32,
    normalized: *const f64,
) {
    if normalized.is_null() && !P::PARAMS.is_empty() {
        return;
    }
    // SAFETY: the caller gives one normalized value per parameter.
    let normalized = unsafe {
        if P::PARAMS.is_empty() {
            &[][..]
        } else {
            slice::from_raw_parts(normalized, P::PARAMS.len())
        }
    };
    // SAFETY: as with says.
    unsafe {
        with::<P, ()>(instance, (), |instance| {
            let size = max_block_size as usize;
            instance.max_block_size = size;
            if P::HAS_AUDIO_INPUT {
                for copy in &mut instance.input_copy {
                    copy.clear();
                    copy.resize(size, 0.0);
                }
            }
            instance.note_copy.reserve(MAX_NOTES);
            instance
                .processor
                .prepare(sample_rate, size, &Params::new(P::PARAMS, normalized));
        });
    }
}

unsafe extern "C" fn latency<P: Processor>(instance: *mut c_void) -> u32 {
    // SAFETY: as with says.
    unsafe { with::<P, u32>(instance, 0, |instance| instance.processor.latency()) }
}

unsafe extern "C" fn tail<P: Processor>(instance: *mut c_void) -> u32 {
    // SAFETY: as with says.
    unsafe {
        with::<P, u32>(instance, 0, |instance| match instance.processor.tail() {
            Tail::Samples(samples) => samples.min(INFINITE_TAIL - 1),
            Tail::Infinite => INFINITE_TAIL,
        })
    }
}

struct Stream<F> {
    function: F,
    stream: *mut c_void,
}

impl StateWriter for Stream<WriteBytes> {
    fn write(&mut self, bytes: &[u8]) -> bool {
        // SAFETY: the C++ side gave the function with the stream it works on.
        unsafe { (self.function)(self.stream, bytes.as_ptr(), bytes.len()) }
    }
}

impl StateReader for Stream<ReadBytes> {
    fn read(&mut self, bytes: &mut [u8]) -> bool {
        // SAFETY: as above, and the buffer is the caller's, of the length given.
        unsafe { (self.function)(self.stream, bytes.as_mut_ptr(), bytes.len()) }
    }
}

unsafe extern "C" fn save_state<P: Processor>(instance: *mut c_void, write: WriteBytes, stream: *mut c_void) -> bool {
    let mut out = Stream {
        function: write,
        stream,
    };
    // A failed instance saves nothing after the parameters, which a project can still be saved with.
    // SAFETY: as with says.
    unsafe { with::<P, bool>(instance, true, |instance| instance.processor.save_state(&mut out)) }
}

unsafe extern "C" fn load_state<P: Processor>(
    instance: *mut c_void,
    normalized: *const f64,
    read: ReadBytes,
    stream: *mut c_void,
) -> bool {
    if normalized.is_null() && !P::PARAMS.is_empty() {
        return false;
    }
    // SAFETY: the caller gives one normalized value per parameter.
    let normalized = unsafe {
        if P::PARAMS.is_empty() {
            &[][..]
        } else {
            slice::from_raw_parts(normalized, P::PARAMS.len())
        }
    };
    let mut input = Stream { function: read, stream };
    // SAFETY: as with says.
    unsafe {
        with::<P, bool>(instance, true, |instance| {
            instance
                .processor
                .load_state(&Params::new(P::PARAMS, normalized), &mut input)
        })
    }
}

unsafe extern "C" fn shared<P: Processor>(instance: *mut c_void) -> *const c_void {
    // SAFETY: as with says.
    unsafe { with::<P, *const c_void>(instance, ptr::null(), |instance| instance.processor.shared()) }
}

unsafe extern "C" fn process<P: Processor>(instance: *mut c_void, block: *const ProcessBlock) -> u32 {
    if block.is_null() {
        return SILENT;
    }
    // SAFETY: the caller gives a filled block, whose pointers stay valid for the call.
    let block = unsafe { &*block };
    let count = block.sample_count as usize;
    if block.output_left.is_null() || block.output_right.is_null() {
        return SILENT;
    }

    // What the instance does with it, outside the unsafe call below, so that each unsafe step in it stands alone.
    let run = |instance: &mut Instance<P>| {
        let max = instance.max_block_size;
        if count == 0 || max == 0 || (block.normalized.is_null() && !P::PARAMS.is_empty()) {
            return Output::Silence;
        }
        // SAFETY: the caller gives one normalized value per parameter, and note_count notes.
        let (normalized, notes) = unsafe {
            (
                if P::PARAMS.is_empty() {
                    &[][..]
                } else {
                    slice::from_raw_parts(block.normalized, P::PARAMS.len())
                },
                if block.notes.is_null() {
                    &[][..]
                } else {
                    slice::from_raw_parts(block.notes, block.note_count as usize)
                },
            )
        };
        let has_input = P::HAS_AUDIO_INPUT && !block.input_left.is_null() && !block.input_right.is_null();
        let Instance {
            processor,
            input_copy,
            note_copy,
            ..
        } = instance;

        // In pieces no longer than the processor was prepared for, which is the whole block from any host that
        // keeps to what it said. The result is silence only when every piece is.
        let mut result = Output::Silence;
        let mut start = 0;
        while start < count {
            let len = (count - start).min(max);

            // The input is copied before any output slice exists, so that the processor's two views never
            // overlap, even when the host's do.
            let input = if has_input {
                let [left, right] = &mut *input_copy;
                // SAFETY: the caller's input buffers hold sample_count samples each, and nothing else reads them.
                unsafe {
                    left[..len].copy_from_slice(slice::from_raw_parts(block.input_left.add(start), len));
                    right[..len].copy_from_slice(slice::from_raw_parts(block.input_right.add(start), len));
                }
                Some([&left[..len], &right[..len]])
            } else {
                None
            };

            let piece_notes = if start == 0 && len == count {
                notes
            } else {
                notes_of_piece(note_copy, notes, start, len, start + len == count)
            };

            // SAFETY: the caller's output buffers hold sample_count samples each, and are the block's to write.
            let output = unsafe {
                [
                    slice::from_raw_parts_mut(block.output_left.add(start), len),
                    slice::from_raw_parts_mut(block.output_right.add(start), len),
                ]
            };
            match processor.process(crate::Block {
                params: Params::new(P::PARAMS, normalized),
                input,
                output,
                transport: &block.transport,
                notes: piece_notes,
            }) {
                Output::Sound => result = Output::Sound,
                // SAFETY: the same piece of the output, with the processor's borrow of it over.
                Output::Silence => unsafe {
                    slice::from_raw_parts_mut(block.output_left.add(start), len).fill(0.0);
                    slice::from_raw_parts_mut(block.output_right.add(start), len).fill(0.0);
                },
            }
            start += len;
        }
        result
    };
    // SAFETY: as with says.
    let result = unsafe { with::<P, Output>(instance, Output::Silence, run) };

    match result {
        Output::Sound => 0,
        Output::Silence => {
            // Whatever the processor left there, including half a block before a panic.
            // SAFETY: the same buffers, with nothing else borrowing them any more.
            unsafe {
                slice::from_raw_parts_mut(block.output_left, count).fill(0.0);
                slice::from_raw_parts_mut(block.output_right, count).fill(0.0);
            }
            SILENT
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Block, Curve, NoteKind, ParamSpec};

    // Its Mode says what it does with a block: doubles the input and marks each piece and note, writes the input
    // backwards, panics, or says it's silent without writing anything.
    struct Recorder {
        calls: usize,
    }

    const RECORDER_PARAMS: [ParamSpec; 1] = [ParamSpec::new(c"Mode", c"", 0.0, 3.0, 0.0, Curve::Integer, 0)];

    impl Processor for Recorder {
        const PARAMS: &'static [ParamSpec] = &RECORDER_PARAMS;
        const HAS_AUDIO_INPUT: bool = true;
        const HAS_NOTE_INPUT: bool = true;

        fn new() -> Self {
            Self { calls: 0 }
        }

        fn prepare(&mut self, _sample_rate: f64, _max_block_size: usize, _params: &Params<'_>) {}

        fn process(&mut self, block: Block<'_>) -> Output {
            let [input_left, _] = block.input.expect("an input");
            let [output_left, output_right] = block.output;
            let len = input_left.len();
            match block.params.normalized(0usize) {
                n if n < 0.25 => {
                    for i in 0..len {
                        output_left[i] = input_left[i] * 2.0;
                        output_right[i] = self.calls as f32;
                    }
                    for note in block.notes {
                        output_right[note.sample_offset as usize] += 10.0 * f32::from(note.pitch);
                    }
                }
                n if n < 0.5 => {
                    for i in 0..len {
                        output_left[i] = input_left[len - 1 - i];
                    }
                }
                n if n < 0.75 => panic!("asked to"),
                _ => return Output::Silence,
            }
            self.calls += 1;
            Output::Sound
        }

        fn latency(&self) -> u32 {
            7
        }
    }

    impl crate::ParamId for usize {
        fn index(self) -> usize {
            self
        }
    }

    const API: PluginApi = PluginApi::of::<Recorder>();

    fn note(sample_offset: i32, pitch: i16) -> NoteEvent {
        NoteEvent {
            kind: NoteKind::On,
            sample_offset,
            pitch,
            channel: 0,
            velocity: 1.0,
            tuning: 0.0,
            note_id: -1,
        }
    }

    struct Buffers {
        input: [Vec<f32>; 2],
        output: [Vec<f32>; 2],
    }

    impl Buffers {
        fn new(count: usize) -> Self {
            let ramp: Vec<f32> = (1..=count).map(|i| i as f32).collect();
            Self {
                input: [ramp.clone(), ramp],
                output: [vec![-1.0; count], vec![-1.0; count]],
            }
        }

        fn block(&mut self, mode: f64, notes: &[NoteEvent], in_place: bool) -> ProcessBlock {
            let normalized = Box::leak(Box::new([mode / 3.0]));
            let count = self.input[0].len() as u32;
            let [input_left, input_right] = &mut self.input;
            let [output_left, output_right] = &mut self.output;
            ProcessBlock {
                normalized: normalized.as_ptr(),
                input_left: input_left.as_ptr(),
                input_right: input_right.as_ptr(),
                output_left: if in_place {
                    input_left.as_mut_ptr()
                } else {
                    output_left.as_mut_ptr()
                },
                output_right: if in_place {
                    input_right.as_mut_ptr()
                } else {
                    output_right.as_mut_ptr()
                },
                sample_count: count,
                transport: Transport::default(),
                notes: notes.as_ptr(),
                note_count: notes.len() as u32,
            }
        }
    }

    fn prepared(max_block_size: u32) -> *mut c_void {
        let instance = unsafe { (API.create)() };
        assert!(!instance.is_null());
        unsafe { (API.prepare)(instance, 48000.0, max_block_size, [0.0f64].as_ptr()) };
        instance
    }

    #[test]
    fn a_block_longer_than_prepared_is_cut_into_pieces_with_their_notes() {
        let instance = prepared(4);
        let mut buffers = Buffers::new(10);
        let notes = [note(1, 60), note(5, 62), note(8, 64), note(9, 65)];
        let block = buffers.block(0.0, &notes, false);
        assert_eq!(unsafe { (API.process)(instance, &block) }, 0);
        assert_eq!(
            buffers.output[0],
            [2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 18.0, 20.0]
        );
        assert_eq!(
            buffers.output[1],
            [0.0, 600.0, 0.0, 0.0, 1.0, 621.0, 1.0, 1.0, 642.0, 652.0]
        );
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn a_block_within_the_prepared_size_is_one_piece() {
        let instance = prepared(16);
        let mut buffers = Buffers::new(10);
        let block = buffers.block(0.0, &[note(3, 61)], false);
        assert_eq!(unsafe { (API.process)(instance, &block) }, 0);
        assert_eq!(buffers.output[1], [0.0, 0.0, 0.0, 610.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]);
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn in_place_processing_sees_the_whole_input_first() {
        let instance = prepared(16);
        let mut buffers = Buffers::new(5);
        let block = buffers.block(1.0, &[], true);
        assert_eq!(unsafe { (API.process)(instance, &block) }, 0);
        assert_eq!(buffers.input[0], [5.0, 4.0, 3.0, 2.0, 1.0]);
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn silence_clears_whatever_was_in_the_output() {
        let instance = prepared(16);
        let mut buffers = Buffers::new(3);
        let block = buffers.block(3.0, &[], false);
        assert_eq!(unsafe { (API.process)(instance, &block) }, SILENT);
        assert_eq!(buffers.output[0], [0.0; 3]);
        assert_eq!(buffers.output[1], [0.0; 3]);
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn a_panic_fails_the_instance_for_good() {
        let instance = prepared(16);
        assert_eq!(unsafe { (API.latency)(instance) }, 7);
        let mut buffers = Buffers::new(3);
        let block = buffers.block(2.0, &[], false);
        assert_eq!(unsafe { (API.process)(instance, &block) }, SILENT);
        assert_eq!(buffers.output[0], [0.0; 3]);

        let block = buffers.block(0.0, &[], false);
        assert_eq!(
            unsafe { (API.process)(instance, &block) },
            SILENT,
            "silent from then on"
        );
        assert_eq!(buffers.output[0], [0.0; 3]);
        assert_eq!(unsafe { (API.latency)(instance) }, 0);
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn a_block_before_prepare_is_silence() {
        let instance = unsafe { (API.create)() };
        let mut buffers = Buffers::new(3);
        let block = buffers.block(0.0, &[], false);
        assert_eq!(unsafe { (API.process)(instance, &block) }, SILENT);
        assert_eq!(buffers.output[0], [0.0; 3]);
        unsafe { (API.destroy)(instance) };
    }

    #[test]
    fn a_null_instance_is_harmless() {
        let mut buffers = Buffers::new(3);
        let block = buffers.block(0.0, &[], false);
        unsafe {
            assert_eq!((API.process)(ptr::null_mut(), &block), SILENT);
            assert_eq!((API.latency)(ptr::null_mut()), 0);
            assert_eq!((API.tail)(ptr::null_mut()), 0);
            (API.prepare)(ptr::null_mut(), 48000.0, 64, [0.0f64].as_ptr());
            (API.destroy)(ptr::null_mut());
        }
        assert_eq!(buffers.output[0], [0.0; 3]);
    }

    #[test]
    fn the_table_describes_the_processor() {
        assert_eq!(API.abi_version, ABI_VERSION);
        assert_eq!(API.param_count, 1);
        const { assert!(API.has_audio_input && API.has_note_input) };
        let mut info = ParamInfo {
            title: ptr::null(),
            units: ptr::null(),
            min: 0.0,
            max: 0.0,
            default_value: 0.0,
            curve: Curve::Linear,
            precision: 0,
            label_count: 0,
            is_silent_at_min: false,
            is_automatable: false,
        };
        assert!(unsafe { (API.param_spec)(0, &mut info) });
        assert_eq!(unsafe { core::ffi::CStr::from_ptr(info.title) }, c"Mode");
        assert_eq!((info.max, info.curve), (3.0, Curve::Integer));
        assert!(!unsafe { (API.param_spec)(1, &mut info) });
        assert!(unsafe { (API.param_label)(0, 0) }.is_null());
        assert_eq!(unsafe { (API.migrate_param)(0, 0.25, 1) }, 0.25);
    }
}
