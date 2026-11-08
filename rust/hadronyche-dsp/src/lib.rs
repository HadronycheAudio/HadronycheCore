//! The signal processing that more than one Hadronyche plug-in needs.
//!
//! Nothing here allocates or touches the operating system, which `no_std` enforces: everything works on slices and
//! iterators that the caller owns, so that all of it can run on an audio thread.

#![no_std]

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
        assert_eq!(peak([&[0.1, -0.5][..], &[0.2, 0.3][..]]), 0.5);
        assert_eq!(peak([&[0.1, 0.2][..], &[-0.9, 0.3][..]]), 0.9);
    }

    #[test]
    fn peak_of_nothing_is_silence() {
        assert_eq!(peak([&[][..], &[][..]]), 0.0);
        let none: [&[f32]; 0] = [];
        assert_eq!(peak(none), 0.0);
    }

    #[test]
    fn peak_ignores_nan_and_keeps_infinity() {
        assert_eq!(peak([&[f32::NAN, 0.25][..]]), 0.25);
        assert_eq!(peak([&[f32::NAN][..]]), 0.0);
        assert_eq!(peak([&[0.5, f32::NEG_INFINITY][..]]), f32::INFINITY);
    }
}
