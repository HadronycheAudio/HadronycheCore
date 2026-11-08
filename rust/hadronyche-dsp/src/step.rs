//! A band-limited step, for anti-aliasing an oscillator's wraps.

/// The integral of a cubic B-spline, minus the ideal step it replaces. `x` is the distance from the step in samples,
/// and the correction reaches zero 2 samples away.
///
/// That reach only works up to a quarter of the sample rate, so an oscillator using this has to fade out by there.
pub fn step_correction(x: f64) -> f64 {
    if x < -1.0 {
        let a = x + 2.0;
        return a * a * a * a / 24.0;
    }
    if x < 0.0 {
        return 0.5 + x * (2.0 / 3.0 - x * x * (1.0 / 3.0 + x / 8.0));
    }
    if x < 1.0 {
        return -0.5 + x * (2.0 / 3.0 - x * x * (1.0 / 3.0 - x / 8.0));
    }

    let a = 2.0 - x;
    -a * a * a * a / 24.0
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same values as tests/CoreTests.cpp has for the C++ one, written out by hand on purpose.
    fn near(value: f64, expected: f64) -> bool {
        (value - expected).abs() <= 1e-12
    }

    #[test]
    fn it_reaches_two_samples_each_side() {
        assert!(near(step_correction(-2.0), 0.0));
        assert!(near(step_correction(-1.0), 1.0 / 24.0));
        assert!(near(step_correction(-1e-15), 0.5));
        assert!(near(step_correction(0.0), -0.5));
        assert!(near(step_correction(1.0), -1.0 / 24.0));
        assert!(near(step_correction(2.0), 0.0));
        assert!(near(
            step_correction(-0.5),
            0.5 - 0.5 * (2.0 / 3.0 - 0.25 * (1.0 / 3.0 - 0.0625))
        ));
    }
}
