//! A gain, as the Rust side of a plug-in: the smallest thing that goes through every part of `hadronyche-plugin`,
//! so that the library's own tests, in tests/RustTests.cpp, can drive the C++ side against it. The Fail switch makes
//! it panic, which is how those tests see that a failed instance goes silent.

use hadronyche_plugin::{
    Block, Curve, Output, ParamSpec, Params, Processor, StateReader, StateWriter, Tail, export_plugin, params,
};

params! {
    pub enum Param => PARAMS {
        Gain => ParamSpec::new(c"Gain", c"dB", -60.0, 6.0, 0.0, Curve::Linear, 1).silent_at_min(),
        Mode => ParamSpec::list(c"Mode", &[c"Stereo", c"Mono"], 0),
        Fail => ParamSpec::toggle(c"Fail", false).not_automatable(),
    }
}

pub struct Example {
    sample_rate: f64,
    /// Saved after the parameters, and counted up by each block of sound, so that the tests can see state go round.
    blocks: u32,
}

impl Processor for Example {
    const PARAMS: &'static [ParamSpec] = &PARAMS;
    // Version 1 saved Gain the other way up.
    const STATE_VERSION: i32 = 2;
    const HAS_AUDIO_INPUT: bool = true;

    fn new() -> Self {
        Self {
            sample_rate: 0.0,
            blocks: 0,
        }
    }

    fn prepare(&mut self, sample_rate: f64, _max_block_size: usize, _params: &Params<'_>) {
        self.sample_rate = sample_rate;
    }

    fn process(&mut self, block: Block<'_>) -> Output {
        if block.params.plain(Param::Fail) > 0.5 {
            panic!("asked to");
        }

        let Some([left, right]) = block.input else {
            return Output::Silence;
        };
        let gain_db = block.params.plain(Param::Gain);
        if gain_db <= -60.0 || hadronyche_dsp::peak([left, right]) == 0.0 {
            return Output::Silence;
        }

        self.blocks += 1;
        let gain = 10f32.powf(gain_db as f32 / 20.0);
        let mono = block.params.plain(Param::Mode) > 0.5;
        let [out_left, out_right] = block.output;
        for (((l, r), o_l), o_r) in left
            .iter()
            .zip(right)
            .zip(out_left.iter_mut())
            .zip(out_right.iter_mut())
        {
            if mono {
                let mid = (l + r) * 0.5 * gain;
                *o_l = mid;
                *o_r = mid;
            } else {
                *o_l = l * gain;
                *o_r = r * gain;
            }
        }
        Output::Sound
    }

    fn latency(&self) -> u32 {
        // Something a test can see change with the sample rate.
        (self.sample_rate / 1000.0) as u32
    }

    fn tail(&self) -> Tail {
        Tail::Infinite
    }

    // The block count, as four bytes after the parameters. A state saved before there was one, such as the tests'
    // hand-written ones, ends before it, and reads as zero.
    fn save_state(&self, out: &mut dyn StateWriter) -> bool {
        out.write(&self.blocks.to_le_bytes())
    }

    fn load_state(&mut self, _params: &Params<'_>, input: &mut dyn StateReader) -> bool {
        let mut bytes = [0u8; 4];
        self.blocks = if input.read(&mut bytes) {
            u32::from_le_bytes(bytes)
        } else {
            0
        };
        true
    }

    fn shared(&self) -> *const core::ffi::c_void {
        (&raw const self.blocks).cast()
    }

    fn migrate_param(index: usize, normalized: f64, version: i32) -> f64 {
        if version < 2 && index == Param::Gain as usize {
            1.0 - normalized
        } else {
            normalized
        }
    }
}

export_plugin!(Example);

#[cfg(test)]
mod tests {
    use super::*;
    use hadronyche_plugin::header;
    use std::path::Path;

    #[test]
    fn param_ids_header_is_current() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/ExampleParamIds.h");
        if let Err(problem) = header::check(&path, &header::param_ids("Example", &Param::NAMES)) {
            panic!("{problem}");
        }
    }

    #[test]
    fn the_api_describes_the_plugin() {
        let api = unsafe { &*hadronyche_plugin() };
        assert_eq!(api.abi_version, hadronyche_plugin::ffi::ABI_VERSION);
        assert_eq!(api.param_count, 3);
        assert_eq!(api.state_version, 2);
        assert!(api.has_audio_input);
        assert!(!api.has_note_input);
    }
}
