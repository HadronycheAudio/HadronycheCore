//! The signal processing that more than one Hadronyche plug-in needs.
//!
//! Everything here runs on an audio thread, so nothing in it allocates or touches the operating system: it works on
//! slices and values that the caller owns. It uses `std` only for the float functions, which call the platform's
//! maths library as the C++ did, so that a port stays bit for bit with what it replaces.

mod gain;
mod power;
mod smoother;
mod step;
mod svf;

pub use gain::{VOLUME_SILENCE_DB, db_to_gain, dry_gain, volume_to_gain, wet_gain};
pub use power::{pow2, pow10};
pub use smoother::Smoother;
pub use step::step_correction;
pub use svf::{SvfCoefficients, SvfMode, SvfState, SvfTaps, svf_coefficients, svf_output, svf_tick, svf_warp};

/// The loudest sample in a block, across any number of channels, ignoring its sign.
///
/// A NaN never wins, so a block of nothing but NaN has a peak of 0, and an infinite sample makes the peak infinite.
pub fn peak<'a>(channels: impl IntoIterator<Item = &'a [f32]>) -> f32 {
    channels
        .into_iter()
        .flatten()
        .map(|sample| sample.abs())
        .fold(0.0, f32::max)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peak_is_the_loudest_sample_of_any_channel() {
        let left = [0.1f32, -0.5, 0.2];
        let right = [0.3f32, 0.4, -0.25];
        assert_eq!(peak([&left[..], &right[..]]), 0.5);
    }

    #[test]
    fn peak_of_nothing_is_silence() {
        let none: [&[f32]; 0] = [];
        assert_eq!(peak(none), 0.0);
        assert_eq!(peak([&[][..]]), 0.0);
    }

    #[test]
    fn peak_ignores_nan_and_keeps_infinity() {
        assert_eq!(peak([&[f32::NAN, 0.25][..]]), 0.25);
        assert_eq!(peak([&[f32::NAN][..]]), 0.0);
        assert_eq!(peak([&[0.5, f32::NEG_INFINITY][..]]), f32::INFINITY);
    }
}
