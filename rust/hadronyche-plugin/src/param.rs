//! A plug-in's parameter table, and the values the host holds for it.

use core::ffi::CStr;

/// How a parameter's 0..1 value maps onto its units. The numbers are those of the C++ `Hadronyche::Curve`, which the
/// controller and the LV2 wrapper still convert with, so they're fixed, and a new curve goes at the end.
///
/// Hosts store values normalized to 0..1, so changing the range or curve of an existing parameter silently changes
/// saved projects.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Curve {
    Linear = 0,
    Log = 1,
    /// Can reach zero, unlike Log, and still gives short times more of the knob's travel.
    Squared = 2,
    List = 3,
    Toggle = 4,
    Integer = 5,
}

/// One parameter, as the host, the panel and the processor all see it. The strings are C strings because the C++
/// side reads them in place.
#[derive(Clone, Copy, Debug)]
pub struct ParamSpec {
    pub title: &'static CStr,
    pub units: &'static CStr,
    pub min: f64,
    pub max: f64,
    /// In plain units, like `min` and `max`.
    pub default: f64,
    pub curve: Curve,
    /// Decimal places in the host's text for it.
    pub precision: i32,
    /// `Curve::List` only, and then `max` is the index of the last one.
    pub labels: &'static [&'static CStr],
    /// Shows the bottom of its range as "-inf", for Volume controls.
    pub is_silent_at_min: bool,
    pub is_automatable: bool,
}

impl ParamSpec {
    pub const fn new(
        title: &'static CStr,
        units: &'static CStr,
        min: f64,
        max: f64,
        default: f64,
        curve: Curve,
        precision: i32,
    ) -> Self {
        Self {
            title,
            units,
            min,
            max,
            default,
            curve,
            precision,
            labels: &[],
            is_silent_at_min: false,
            is_automatable: true,
        }
    }

    /// A choice from a list, saved by position, so only ever add to the end of the labels.
    pub const fn list(title: &'static CStr, labels: &'static [&'static CStr], default: usize) -> Self {
        let mut spec = Self::new(
            title,
            c"",
            0.0,
            (labels.len() - 1) as f64,
            default as f64,
            Curve::List,
            0,
        );
        spec.labels = labels;
        spec
    }

    pub const fn toggle(title: &'static CStr, on: bool) -> Self {
        Self::new(title, c"", 0.0, 1.0, if on { 1.0 } else { 0.0 }, Curve::Toggle, 0)
    }

    pub const fn silent_at_min(mut self) -> Self {
        self.is_silent_at_min = true;
        self
    }

    pub const fn not_automatable(mut self) -> Self {
        self.is_automatable = false;
        self
    }

    /// The host's 0..1 value in this parameter's units. The same arithmetic as the C++ `normalizedToPlain`, so that
    /// the processor and the controller agree to the last bit.
    pub fn normalized_to_plain(&self, normalized: f64) -> f64 {
        let n = clamp(normalized, 0.0, 1.0);
        match self.curve {
            Curve::Linear => self.min + (self.max - self.min) * n,
            Curve::Log => self.min * (self.max / self.min).powf(n),
            Curve::Squared => self.min + (self.max - self.min) * n * n,
            Curve::List => (n * (self.labels.len() as f64 - 1.0)).round(),
            Curve::Toggle => {
                if n >= 0.5 {
                    1.0
                } else {
                    0.0
                }
            }
            Curve::Integer => (self.min + (self.max - self.min) * n).round(),
        }
    }

    /// A value in this parameter's units as the host's 0..1 value.
    pub fn plain_to_normalized(&self, plain: f64) -> f64 {
        let p = clamp(plain, self.min, self.max);
        match self.curve {
            Curve::Log => (p / self.min).ln() / (self.max / self.min).ln(),
            Curve::Squared => ((p - self.min) / (self.max - self.min)).sqrt(),
            Curve::Linear | Curve::List | Curve::Toggle | Curve::Integer => (p - self.min) / (self.max - self.min),
        }
    }
}

/// What `std::clamp` does, including letting NaN through, and without `f64::clamp`'s panic on a bad range.
fn clamp(value: f64, low: f64, high: f64) -> f64 {
    if value < low {
        low
    } else if value > high {
        high
    } else {
        value
    }
}

/// A parameter's ID, which [`params!`](crate::params) implements for a plug-in's enum.
pub trait ParamId: Copy {
    fn index(self) -> usize;
}

/// The host's values for a plug-in's parameters, as a block sees them: one normalized value for each, in ID order,
/// with the table to convert them.
#[derive(Clone, Copy)]
pub struct Params<'a> {
    specs: &'static [ParamSpec],
    normalized: &'a [f64],
}

impl<'a> Params<'a> {
    /// `normalized` has one value per spec, which the C++ side guarantees.
    pub fn new(specs: &'static [ParamSpec], normalized: &'a [f64]) -> Self {
        debug_assert_eq!(specs.len(), normalized.len());
        Self { specs, normalized }
    }

    pub fn normalized(&self, id: impl ParamId) -> f64 {
        self.normalized[id.index()]
    }

    /// The value in the parameter's own units.
    pub fn plain(&self, id: impl ParamId) -> f64 {
        let index = id.index();
        self.specs[index].normalized_to_plain(self.normalized[index])
    }

    pub fn spec(&self, id: impl ParamId) -> &'static ParamSpec {
        &self.specs[id.index()]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The same expected values as tests/CoreTests.cpp has for the C++ conversions, written out by hand on purpose: a
    // failure means that the processor would read a saved project differently from the controller.
    const LINEAR: ParamSpec = ParamSpec::new(c"Level", c"dB", -60.0, 6.0, 0.0, Curve::Linear, 1);
    const LOG: ParamSpec = ParamSpec::new(c"Time", c"ms", 10.0, 10000.0, 100.0, Curve::Log, 0);
    const SQUARED: ParamSpec = ParamSpec::new(c"Attack", c"ms", 0.0, 10.0, 1.0, Curve::Squared, 1);
    const LIST: ParamSpec = ParamSpec::list(c"Choice", &[c"One", c"Two", c"Three", c"Four", c"Five"], 1);
    const TOGGLE: ParamSpec = ParamSpec::toggle(c"Switch", true);
    const INTEGER: ParamSpec = ParamSpec::new(c"Bars", c"bars", 1.0, 64.0, 16.0, Curve::Integer, 0);

    fn near(value: f64, expected: f64, tolerance: f64) -> bool {
        (value - expected).abs() <= tolerance
    }

    #[test]
    fn curves_match_the_c_plus_plus_conversions() {
        assert_eq!(LINEAR.normalized_to_plain(0.5), -27.0);
        assert_eq!(LINEAR.plain_to_normalized(-27.0), 0.5);
        assert!(near(LOG.normalized_to_plain(0.5), 316.227766016838, 1e-9));
        assert!(near(LOG.plain_to_normalized(100.0), 1.0 / 3.0, 1e-12));
        assert_eq!(SQUARED.normalized_to_plain(0.5), 2.5);
        assert_eq!(SQUARED.plain_to_normalized(2.5), 0.5);
        assert_eq!(LIST.normalized_to_plain(0.37), 1.0);
        assert_eq!(LIST.normalized_to_plain(0.375), 2.0);
        assert_eq!(LIST.plain_to_normalized(3.0), 0.75);
        assert_eq!(TOGGLE.normalized_to_plain(0.49), 0.0);
        assert_eq!(TOGGLE.normalized_to_plain(0.5), 1.0);
        assert_eq!(INTEGER.normalized_to_plain(0.5), 33.0);
        assert_eq!(INTEGER.plain_to_normalized(32.5), 0.5);
    }

    #[test]
    fn values_are_held_to_the_range() {
        assert_eq!(LINEAR.normalized_to_plain(2.0), 6.0);
        assert_eq!(LINEAR.normalized_to_plain(-1.0), -60.0);
        assert_eq!(LINEAR.plain_to_normalized(100.0), 1.0);
        assert_eq!(LINEAR.plain_to_normalized(-100.0), 0.0);
    }

    #[test]
    fn a_list_knows_its_range_and_default() {
        assert_eq!(LIST.min, 0.0);
        assert_eq!(LIST.max, 4.0);
        assert_eq!(LIST.default, 1.0);
        assert_eq!(LIST.labels.len(), 5);
    }

    #[test]
    fn params_convert_by_id() {
        #[derive(Clone, Copy)]
        enum Id {
            Level,
            Time,
        }
        impl ParamId for Id {
            fn index(self) -> usize {
                self as usize
            }
        }
        const SPECS: [ParamSpec; 2] = [LINEAR, LOG];
        let params = Params::new(&SPECS, &[0.5, 0.0]);
        assert_eq!(params.plain(Id::Level), -27.0);
        assert_eq!(params.plain(Id::Time), 10.0);
        assert_eq!(params.normalized(Id::Level), 0.5);
        assert_eq!(params.spec(Id::Time).title, c"Time");
    }
}
