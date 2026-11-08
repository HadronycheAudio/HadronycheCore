//! Headers that the C++ side gets from the Rust side, kept in the repository and checked by `cargo test`.
//!
//! A plug-in's test calls [`check`] with its [`param_ids`] header, and this crate's own test does the same with the
//! header that cbindgen makes of [`ffi`](crate::ffi). A test fails when the file in the repository is out of date,
//! and `HADRONYCHE_UPDATE_HEADERS=1 cargo test` rewrites it.

use std::fmt::Write;
use std::path::Path;

/// The environment variable that makes [`check`] write the header instead of comparing it.
pub const UPDATE_VARIABLE: &str = "HADRONYCHE_UPDATE_HEADERS";

/// A C++ header with a plug-in's parameter IDs, as the C++ side has always written them: `enum ParamId` in the
/// plug-in's namespace, `kName` for each, and `kParamCount` last. `names` are the enum's variants from
/// [`params!`](crate::params).
pub fn param_ids(namespace: &str, names: &[&str]) -> String {
    let mut text = String::new();
    text.push_str("#pragma once\n\n");
    text.push_str(
        "// Generated from the parameter table in rust/, by cargo test there. Don't edit it here: change the table,\n",
    );
    text.push_str("// and run HADRONYCHE_UPDATE_HEADERS=1 cargo test in rust/ to bring this up to date.\n\n");
    text.push_str("#include \"pluginterfaces/vst/vsttypes.h\"\n\n");
    let _ = writeln!(text, "namespace {namespace} {{\n");
    text.push_str("// Parameter IDs are saved in projects and automation: never renumber them, only append.\n");
    text.push_str("enum ParamId : Steinberg::Vst::ParamID\n{\n");
    for name in names {
        let _ = writeln!(text, "    k{name},");
    }
    text.push_str("    kParamCount\n};\n\n");
    let _ = writeln!(text, "}} // namespace {namespace}");
    text
}

/// Compares `expected` with the file at `path`, and returns what's wrong when they differ, or writes the file when
/// [`UPDATE_VARIABLE`] is set. Line endings don't count, because git may change them on the way to Windows.
pub fn check(path: &Path, expected: &str) -> Result<(), String> {
    check_or_update(path, expected, std::env::var_os(UPDATE_VARIABLE).is_some())
}

fn check_or_update(path: &Path, expected: &str, update: bool) -> Result<(), String> {
    if update {
        return std::fs::write(path, expected).map_err(|error| format!("couldn't write {}: {error}", path.display()));
    }
    let actual = std::fs::read_to_string(path).map_err(|error| format!("couldn't read {}: {error}", path.display()))?;
    if actual.replace("\r\n", "\n") == expected.replace("\r\n", "\n") {
        Ok(())
    } else {
        Err(format!(
            "{} is out of date with the Rust side. Run {UPDATE_VARIABLE}=1 cargo test in rust/ to bring it up to date",
            path.display()
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn param_ids_header_names_each_id_and_the_count() {
        let text = param_ids("Example", &["Gain", "DryWet"]);
        assert!(text.starts_with("#pragma once\n"));
        assert!(text.contains("namespace Example {\n"));
        assert!(
            text.contains("enum ParamId : Steinberg::Vst::ParamID\n{\n    kGain,\n    kDryWet,\n    kParamCount\n};\n")
        );
        assert!(text.ends_with("} // namespace Example\n"));
    }

    #[test]
    fn check_tells_the_difference_from_the_file() {
        let folder = std::env::temp_dir().join(format!("hadronyche-header-{}", std::process::id()));
        std::fs::create_dir_all(&folder).unwrap();
        let path = folder.join("Header.h");

        assert!(
            check_or_update(&path, "a\n", false).is_err(),
            "a missing file is out of date"
        );
        std::fs::write(&path, "a\r\n").unwrap();
        assert!(check_or_update(&path, "a\n", false).is_ok(), "line endings don't count");
        std::fs::write(&path, "b\n").unwrap();
        assert!(
            check_or_update(&path, "a\n", false)
                .unwrap_err()
                .contains(UPDATE_VARIABLE)
        );
        assert!(check_or_update(&path, "a\n", true).is_ok(), "updating writes it");
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "a\n");

        std::fs::remove_dir_all(&folder).unwrap();
    }
}
