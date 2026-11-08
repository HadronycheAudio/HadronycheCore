//! The kernel of a topology-preserving state-variable filter, which stays stable under fast modulation. Plug-ins
//! build their own filters on it, with however many channels and stages they need.

/// Plug-ins cast saved parameter values to this, so only ever add to the end.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SvfMode {
    LowPass = 0,
    HighPass = 1,
    BandPass = 2,
    Notch = 3,
}

#[derive(Clone, Copy, Debug)]
pub struct SvfCoefficients {
    pub k: f32,
    pub a1: f32,
    pub a2: f32,
    pub a3: f32,
}

impl Default for SvfCoefficients {
    fn default() -> Self {
        Self {
            k: 1.0,
            a1: 0.0,
            a2: 0.0,
            a3: 0.0,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SvfState {
    pub ic1: f32,
    pub ic2: f32,
}

#[derive(Clone, Copy, Debug)]
pub struct SvfTaps {
    pub v1: f32,
    pub v2: f32,
}

/// The prewarped frequency, held to what the filter can do.
pub fn svf_warp(frequency: f64, sample_rate: f64) -> f64 {
    let limited = frequency.clamp(10.0, sample_rate * 0.49);
    (core::f64::consts::PI * limited / sample_rate).tan()
}

pub fn svf_coefficients(g: f64, q: f64) -> SvfCoefficients {
    let k = 1.0 / q;
    let a1 = 1.0 / (1.0 + g * (g + k));
    let a2 = g * a1;
    SvfCoefficients {
        k: k as f32,
        a1: a1 as f32,
        a2: a2 as f32,
        a3: (g * a2) as f32,
    }
}

pub fn svf_tick(input: f32, c: &SvfCoefficients, state: &mut SvfState) -> SvfTaps {
    let v3 = input - state.ic2;
    let v1 = c.a1 * state.ic1 + c.a2 * v3;
    let v2 = state.ic2 + c.a2 * state.ic1 + c.a3 * v3;
    state.ic1 = 2.0 * v1 - state.ic1;
    state.ic2 = 2.0 * v2 - state.ic2;
    SvfTaps { v1, v2 }
}

pub fn svf_output(mode: SvfMode, input: f32, k: f32, taps: SvfTaps) -> f32 {
    match mode {
        SvfMode::LowPass => taps.v2,
        SvfMode::HighPass => input - k * taps.v1 - taps.v2,
        // Scaled so the peak stays at unity as resonance rises.
        SvfMode::BandPass => k * taps.v1,
        SvfMode::Notch => input - k * taps.v1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same values as tests/CoreTests.cpp has for the C++ one, written out by hand on purpose.
    fn near(value: f64, expected: f64, tolerance: f64) -> bool {
        (value - expected).abs() <= tolerance
    }

    #[test]
    fn the_frequency_is_held_to_what_the_filter_can_do() {
        assert_eq!(svf_warp(1.0, 48000.0), (core::f64::consts::PI * 10.0 / 48000.0).tan());
        assert_eq!(svf_warp(96000.0, 48000.0), (core::f64::consts::PI * 0.49).tan());
    }

    #[test]
    fn one_step_by_hand() {
        let c = svf_coefficients(1.0, 0.5);
        assert!(near(c.k.into(), 2.0, 1e-6));
        assert!(near(c.a1.into(), 0.25, 1e-6));
        assert!(near(c.a2.into(), 0.25, 1e-6));
        assert!(near(c.a3.into(), 0.25, 1e-6));

        let mut state = SvfState::default();
        let taps = svf_tick(1.0, &c, &mut state);
        assert!(near(taps.v1.into(), 0.25, 1e-6));
        assert!(near(taps.v2.into(), 0.25, 1e-6));
        assert!(near(state.ic1.into(), 0.5, 1e-6));
        assert!(near(state.ic2.into(), 0.5, 1e-6));

        assert!(near(
            svf_output(SvfMode::LowPass, 1.0, 2.0, SvfTaps { v1: 0.25, v2: 0.5 }).into(),
            0.5,
            1e-6
        ));
        assert!(near(
            svf_output(SvfMode::HighPass, 1.0, 2.0, SvfTaps { v1: 0.25, v2: 0.25 }).into(),
            0.25,
            1e-6
        ));
        assert!(near(
            svf_output(SvfMode::BandPass, 1.0, 2.0, SvfTaps { v1: 0.25, v2: 0.5 }).into(),
            0.5,
            1e-6
        ));
        assert!(near(
            svf_output(SvfMode::Notch, 1.0, 2.0, SvfTaps { v1: 0.25, v2: 0.5 }).into(),
            0.5,
            1e-6
        ));
    }

    #[test]
    fn the_modes_values_are_frozen() {
        assert_eq!(SvfMode::LowPass as i32, 0);
        assert_eq!(SvfMode::HighPass as i32, 1);
        assert_eq!(SvfMode::BandPass as i32, 2);
        assert_eq!(SvfMode::Notch as i32, 3);
    }

    #[test]
    fn dc_comes_through_the_low_pass_and_not_the_high_pass() {
        let low_pass = svf_coefficients(svf_warp(1000.0, 48000.0), 0.707);
        let mut settled = SvfState::default();
        let mut last = SvfTaps { v1: 0.0, v2: 0.0 };
        for _ in 0..48000 {
            last = svf_tick(1.0, &low_pass, &mut settled);
        }
        assert!(near(
            svf_output(SvfMode::LowPass, 1.0, low_pass.k, last).into(),
            1.0,
            1e-3
        ));
        assert!(near(
            svf_output(SvfMode::HighPass, 1.0, low_pass.k, last).into(),
            0.0,
            1e-3
        ));
    }
}
