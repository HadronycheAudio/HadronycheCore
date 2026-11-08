//! One-pole smoothing, which removes clicks and zipper noise from parameter changes.

/// A value that glides to its target, one call to [`step`](Self::step) at a time, the same way as the C++
/// `Hadronyche::Smoother`.
///
/// It has to stay double precision. In single precision, the steps become too small to register before reaching the
/// target, and it stalls about 0.01% short, which leaves a held note out of tune after a glide and makes a free
/// running loop the wrong length.
#[derive(Clone, Copy, Debug, Default)]
pub struct Smoother {
    coefficient: f64,
    target: f64,
    current: f64,
}

impl Smoother {
    /// `rate` is how often `step` is called, per second, and `time_ms` the time constant.
    pub fn prepare(&mut self, rate: f64, time_ms: f64) {
        self.coefficient = (-1000.0 / (rate * time_ms)).exp();
    }

    pub fn set_target(&mut self, target: f64) {
        self.target = target;
    }

    pub fn snap_to_target(&mut self) {
        self.current = self.target;
    }

    pub fn snap_to(&mut self, value: f64) {
        self.target = value;
        self.current = value;
    }

    pub fn step(&mut self) -> f64 {
        self.current = self.target + (self.current - self.target) * self.coefficient;
        self.current
    }

    /// For single precision signal paths: the result goes through `f32`, so that an expression using it stays in
    /// `f32`, which a plain `step` would change.
    pub fn step_f32(&mut self) -> f32 {
        self.step() as f32
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
    fn one_step_covers_one_time_constant() {
        let mut smoother = Smoother::default();
        smoother.prepare(1000.0, 1.0);
        smoother.set_target(1.0);
        assert!(near(smoother.step(), 0.632120558828558, 1e-12));
        assert!(near(smoother.step(), 0.864664716763387, 1e-12));

        smoother.snap_to_target();
        assert_eq!(smoother.step(), 1.0);

        smoother.snap_to(0.25);
        assert_eq!(smoother.step(), 0.25);
    }

    #[test]
    fn it_gets_all_the_way_to_its_target() {
        let mut slow = Smoother::default();
        slow.prepare(48000.0, 100.0);
        slow.set_target(1.0);
        let mut value = 0.0;
        for _ in 0..480000 {
            value = slow.step();
        }
        assert!(near(value, 1.0, 1e-12));
    }

    #[test]
    fn step_f32_is_a_float() {
        let mut single = Smoother::default();
        single.prepare(1000.0, 1.0);
        single.set_target(1.0);
        // The same value as step gives, rounded to f32 once.
        let expected: f64 = 0.632120558828558;
        assert_eq!(single.step_f32(), expected as f32);
    }
}
