//! Powers of 2 and 10, as the C library's `pow` computes them.
//!
//! LLVM turns `pow (2, x)` into `exp2 (x)`, and could turn `pow (10, x)` into `exp10 (x)`, because they're the same
//! in mathematics. In glibc they aren't the same in the last bit, now and then, and the C++ this replaces called
//! `pow`, so a port that used `2f64.powf (x)` stopped being bit for bit at a few samples in a hundred thousand.
//! Hiding the base from the optimiser keeps the call to `pow`.

use core::hint::black_box;

pub fn pow2(x: f64) -> f64 {
    black_box(2.0f64).powf(x)
}

pub fn pow10(x: f64) -> f64 {
    black_box(10.0f64).powf(x)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn they_are_powers() {
        assert_eq!(pow2(10.0), 1024.0);
        assert_eq!(pow2(-1.0), 0.5);
        assert_eq!(pow10(3.0), 1000.0);
        assert_eq!(pow10(-2.0), 0.01);
    }
}
