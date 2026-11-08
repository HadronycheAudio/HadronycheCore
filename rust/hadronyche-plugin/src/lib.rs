//! The frame that a plug-in's Rust processor fits into.
//!
//! A plug-in describes its parameters with [`params!`], implements [`Processor`] for its signal processing, and ends
//! with [`export_plugin!`], which exports the one C symbol, `hadronyche_plugin`, that the C++ side in
//! `include/hadronyche/vst/RustProcessor.h` calls everything through. The C side of that is in [`ffi`], and the
//! header for it, `include/hadronyche/ffi/Plugin.h`, is generated from this crate by `cargo test`.
//!
//! The C++ side keeps everything a host sees: the VST3 classes, the saved state's bytes, the controller, the panel
//! and the LV2 wrapper. What crosses into Rust is the parameters' normalized values, the audio, the transport and
//! the notes, and what comes back is the audio and whether it's silent. A panic anywhere in a processor is caught at
//! the boundary, and that instance stays silent from then on.

pub mod ffi;
pub mod header;
mod param;
mod processor;

pub use param::{Curve, ParamId, ParamSpec, Params};
pub use processor::{
    Block, NoteEvent, NoteKind, Output, Processor, StateReader, StateWriter, TRANSPORT_BAR_POSITION_VALID,
    TRANSPORT_PLAYING, TRANSPORT_PROJECT_TIME_MUSIC_VALID, TRANSPORT_TEMPO_VALID, TRANSPORT_TIME_SIGNATURE_VALID, Tail,
    Transport,
};

/// A plug-in's parameters: an enum of their IDs, in the order the host sees and projects save, each with its
/// [`ParamSpec`], and the table of those specs under the second name.
///
/// ```
/// use hadronyche_plugin::{Curve, ParamSpec, params};
///
/// params! {
///     pub enum Param => PARAMS {
///         Gain => ParamSpec::new(c"Gain", c"dB", -60.0, 6.0, 0.0, Curve::Linear, 1),
///         Mode => ParamSpec::list(c"Mode", &[c"Soft", c"Hard"], 0),
///     }
/// }
///
/// assert_eq!(Param::COUNT, 2);
/// assert_eq!(Param::NAMES, ["Gain", "Mode"]);
/// assert_eq!(PARAMS[Param::Mode as usize].max, 1.0);
/// ```
///
/// IDs are saved in projects and automation, so never reorder or remove a parameter, only add one at the end.
#[macro_export]
macro_rules! params {
    (
        $(#[$meta:meta])*
        $vis:vis enum $name:ident => $table:ident {
            $( $variant:ident => $spec:expr ),+ $(,)?
        }
    ) => {
        $(#[$meta])*
        #[repr(u32)]
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        $vis enum $name {
            $( $variant ),+
        }

        impl $name {
            pub const COUNT: usize = [$( Self::$variant ),+].len();
            pub const NAMES: [&'static str; Self::COUNT] = [$( stringify!($variant) ),+];
        }

        impl $crate::ParamId for $name {
            fn index(self) -> usize {
                self as usize
            }
        }

        $vis const $table: [$crate::ParamSpec; $name::COUNT] = [$( $spec ),+];
    };
}

/// Exports a [`Processor`] as the plug-in in this library, through the symbol `hadronyche_plugin`, which hands the
/// C++ side its table of functions. Each binary holds one plug-in, so the name is the same in all of them.
#[macro_export]
macro_rules! export_plugin {
    ($processor:ty) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn hadronyche_plugin() -> *const $crate::ffi::PluginApi {
            static API: $crate::ffi::PluginApi = $crate::ffi::PluginApi::of::<$processor>();
            &API
        }
    };
}
