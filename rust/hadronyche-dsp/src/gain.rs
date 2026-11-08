//! Gain laws, the same as the C++ `hadronyche/dsp/Gain.h`.

/// At or below this, a Volume control is silent.
pub const VOLUME_SILENCE_DB: f64 = -60.0;

pub fn db_to_gain(db: f64) -> f64 {
    crate::pow10(db / 20.0)
}

pub fn volume_to_gain(db: f64) -> f64 {
    if db <= VOLUME_SILENCE_DB { 0.0 } else { db_to_gain(db) }
}

/// A dry/wet law where both play at full level at 50%, so that nothing drops in level in the middle.
pub fn dry_gain(percent: f64) -> f64 {
    (2.0 * (1.0 - percent / 100.0)).min(1.0)
}

pub fn wet_gain(percent: f64) -> f64 {
    (2.0 * percent / 100.0).min(1.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same values as tests/CoreTests.cpp has for the C++ ones, written out by hand on purpose.
    fn near(value: f64, expected: f64) -> bool {
        (value - expected).abs() <= 1e-12
    }

    #[test]
    fn decibels() {
        assert!(near(db_to_gain(0.0), 1.0));
        assert!(near(db_to_gain(-20.0), 0.1));
        assert!(near(db_to_gain(6.0), 1.99526231496888));
    }

    #[test]
    fn volume_is_silent_at_and_below_its_floor() {
        assert_eq!(VOLUME_SILENCE_DB, -60.0);
        assert_eq!(volume_to_gain(-60.0), 0.0);
        assert_eq!(volume_to_gain(-90.0), 0.0);
        assert!(near(volume_to_gain(-59.0), 0.00112201845430196));
    }

    #[test]
    fn dry_and_wet_are_both_full_in_the_middle() {
        assert!(near(dry_gain(0.0), 1.0));
        assert!(near(dry_gain(50.0), 1.0));
        assert!(near(dry_gain(75.0), 0.5));
        assert!(near(dry_gain(100.0), 0.0));
        assert!(near(wet_gain(0.0), 0.0));
        assert!(near(wet_gain(25.0), 0.5));
        assert!(near(wet_gain(100.0), 1.0));
    }
}
