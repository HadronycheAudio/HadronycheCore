//! include/hadronyche/ffi/Plugin.h is what cbindgen makes of this crate, so that the C++ side can never disagree
//! with the Rust side about the structs and functions between them.

use std::path::Path;

#[test]
fn plugin_header_is_current() {
    let crate_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let header = cbindgen::Builder::new()
        .with_crate(crate_dir)
        .with_config(cbindgen::Config::from_file(crate_dir.join("cbindgen.toml")).expect("cbindgen.toml reads"))
        .generate()
        .expect("cbindgen makes the header");
    let mut text = Vec::new();
    header.write(&mut text);
    let text = String::from_utf8(text).expect("the header is text");

    let path = crate_dir.join("../../include/hadronyche/ffi/Plugin.h");
    if let Err(problem) = hadronyche_plugin::header::check(&path, &text) {
        panic!("{problem}");
    }
}
