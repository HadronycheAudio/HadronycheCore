# A plug-in's Rust side, which Cargo builds from its rust/Cargo.toml into a static library. A plug-in calls
#
#   hadronyche_add_rust (MyPlugin)
#
# before hadronyche_add_plugin, which links the target <name>-rust into the VST3, as hadronyche_add_lv2 does into the
# LV2. The crate is a staticlib called <name> in lower case, and the symbols it exports are declared by hand on the
# C++ side. It needs Rust from https://rustup.rs, and on macOS both targets: rustup target add aarch64-apple-darwin
# x86_64-apple-darwin.
#
# The crate depends on the library's crates by their path in the submodule, because Cargo can't redirect a path
# dependency, so the Rust side always builds from external/hadronychecore. With HADRONYCHE_CORE_DIR pointing elsewhere,
# the two have to hold the same rust/ folder, and this stops with an error when they don't.

function(hadronyche_add_rust name)
    string(TOLOWER "${name}" crate)
    set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "hadronyche_add_rust: ${manifest} is missing")
    endif()

    _hadronyche_rust_check_submodule()
    _hadronyche_rust_find_tools()

    # The plug-in is a universal binary on macOS, built by Xcode from its standard architectures, and Rust has no
    # universal target, so it builds twice and lipo joins the slices.
    if(APPLE)
        set(triples aarch64-apple-darwin x86_64-apple-darwin)
    else()
        set(triples "${HADRONYCHE_RUST_HOST}")
    endif()

    set(environment)
    if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND environment "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    # The same C runtime as the plug-in's, as SMTG_USE_STATIC_CRT in HadronychePlugin.cmake explains. Rust only has the
    # release one, and the plug-in's link picks its own, so the runtime is left out of the libraries below.
    if(MSVC AND SMTG_USE_STATIC_CRT)
        list(APPEND environment "RUSTFLAGS=-C target-feature=+crt-static")
    endif()

    # Cargo's dev profile for a Debug build, and its release one for anything else. Each configuration gets its own
    # copy of the result, because a multi-configuration generator builds several from one tree.
    set(profile "$<IF:$<CONFIG:Debug>,dev,release>")
    set(profile_dir "$<IF:$<CONFIG:Debug>,debug,release>")
    set(target_dir "${CMAKE_BINARY_DIR}/cargo")
    if(WIN32)
        set(file "${crate}.lib")
    else()
        set(file "lib${crate}.a")
    endif()
    set(output_dir "${CMAKE_BINARY_DIR}/rust/$<CONFIG>")
    set(output "${output_dir}/${file}")

    set(commands COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}")
    set(slices)
    foreach(triple ${triples})
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E env ${environment}
            "${HADRONYCHE_CARGO}" build --manifest-path "${manifest}" --target ${triple} --target-dir "${target_dir}" --profile ${profile})
        list(APPEND slices "${target_dir}/${triple}/${profile_dir}/${file}")
    endforeach()
    list(LENGTH triples triple_count)
    if(triple_count GREATER 1)
        list(APPEND commands COMMAND lipo -create -output "${output}" ${slices})
    else()
        list(APPEND commands COMMAND ${CMAKE_COMMAND} -E copy_if_different ${slices} "${output}")
    endif()

    # Cargo tracks the crate's own sources, so it runs on every build and does nothing when nothing changed.
    add_custom_target(${name}-rust-build
        ${commands}
        BYPRODUCTS "${output}"
        COMMENT "Building the Rust side of ${name}"
        VERBATIM
    )

    _hadronyche_rust_native_libraries(native_libraries)
    add_library(${name}-rust INTERFACE)
    target_link_libraries(${name}-rust INTERFACE "${output}" ${native_libraries})
    add_dependencies(${name}-rust ${name}-rust-build)
endfunction()

# Stops when HADRONYCHE_CORE_DIR's rust/ isn't the submodule's, which is what the Rust side builds from.
function(_hadronyche_rust_check_submodule)
    set(submodule "${CMAKE_CURRENT_SOURCE_DIR}/external/hadronychecore")
    get_filename_component(core_real "${HADRONYCHE_CORE_DIR}" REALPATH)
    get_filename_component(submodule_real "${submodule}" REALPATH)
    if(core_real STREQUAL submodule_real)
        return()
    endif()

    file(GLOB_RECURSE core_files RELATIVE "${core_real}/rust" "${core_real}/rust/*")
    file(GLOB_RECURSE submodule_files RELATIVE "${submodule_real}/rust" "${submodule_real}/rust/*")
    list(FILTER core_files EXCLUDE REGEX "^target/")
    list(FILTER submodule_files EXCLUDE REGEX "^target/")
    set(same TRUE)
    if(NOT core_files STREQUAL submodule_files)
        set(same FALSE)
    else()
        foreach(file ${core_files})
            file(SHA256 "${core_real}/rust/${file}" core_hash)
            file(SHA256 "${submodule_real}/rust/${file}" submodule_hash)
            if(NOT core_hash STREQUAL submodule_hash)
                set(same FALSE)
                break()
            endif()
        endforeach()
    endif()
    if(NOT same)
        message(FATAL_ERROR
            "HADRONYCHE_CORE_DIR is ${HADRONYCHE_CORE_DIR}, but its rust/ differs from the submodule's, and the Rust "
            "side can only build from the submodule. Point the submodule at that commit:\n"
            "  git -C external/hadronychecore fetch ${HADRONYCHE_CORE_DIR} <branch>\n"
            "  git -C external/hadronychecore checkout FETCH_HEAD")
    endif()
endfunction()

# HADRONYCHE_CARGO, HADRONYCHE_RUSTC and HADRONYCHE_RUST_HOST, the host's target triple.
macro(_hadronyche_rust_find_tools)
    set(hints "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    find_program(HADRONYCHE_CARGO cargo HINTS ${hints} DOC "Cargo, which builds a plug-in's Rust side")
    find_program(HADRONYCHE_RUSTC rustc HINTS ${hints} DOC "The Rust compiler that Cargo uses")
    if(NOT HADRONYCHE_CARGO OR NOT HADRONYCHE_RUSTC)
        message(FATAL_ERROR "Rust is missing. Install it from https://rustup.rs")
    endif()
    if(NOT HADRONYCHE_RUST_HOST)
        execute_process(COMMAND "${HADRONYCHE_RUSTC}" -vV OUTPUT_VARIABLE rustc_version RESULT_VARIABLE rustc_result)
        if(rustc_result OR NOT rustc_version MATCHES "host: ([^\r\n]+)")
            message(FATAL_ERROR "rustc -vV didn't say what the host is: ${rustc_version}")
        endif()
        set(HADRONYCHE_RUST_HOST "${CMAKE_MATCH_1}" CACHE INTERNAL "The Rust target triple of this machine")
    endif()
endmacro()

# The system libraries that Rust's standard library needs, which rustc knows and the plug-in's link has to add. Asked
# of a throwaway crate, the way Corrosion does it.
function(_hadronyche_rust_native_libraries out_libraries)
    set(probe_dir "${CMAKE_BINARY_DIR}/rust/probe")
    file(MAKE_DIRECTORY "${probe_dir}/src")
    file(WRITE "${probe_dir}/Cargo.toml"
        "[package]\nname = \"probe\"\nversion = \"0.0.0\"\nedition = \"2021\"\n\n[lib]\ncrate-type = [\"staticlib\"]\n\n[workspace]\n")
    file(WRITE "${probe_dir}/src/lib.rs" "pub fn probe() {}\n")
    # Without colour, because CI sets CARGO_TERM_COLOR=always, and the escape codes would end up in the link.
    execute_process(
        COMMAND "${HADRONYCHE_CARGO}" rustc --quiet --color never --manifest-path "${probe_dir}/Cargo.toml"
            --target ${HADRONYCHE_RUST_HOST} --target-dir "${probe_dir}/target" -- --print=native-static-libs
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    if(result OR NOT errors MATCHES "native-static-libs: ([^\r\n]*)")
        message(FATAL_ERROR "rustc didn't say which libraries it needs: ${errors}")
    endif()
    set(list "${CMAKE_MATCH_1}")
    string(REPLACE " " ";" libraries "${list}")

    set(result_libraries)
    set(framework_next FALSE)
    foreach(library ${libraries})
        # A framework is two words, which CMake's link keeps together as one.
        if(library STREQUAL "-framework")
            set(framework_next TRUE)
            continue()
        elseif(framework_next)
            list(APPEND result_libraries "-framework ${library}")
            set(framework_next FALSE)
            continue()
        endif()
        # The plug-in's link picks the C runtime, which MSVC's list names as a flag, and on Linux it links the GCC
        # runtime statically, which has the unwinder that gcc_s would bring in a second time.
        string(REGEX REPLACE "^-l" "" library "${library}")
        string(REGEX REPLACE "\\.lib$" "" library "${library}")
        if(library MATCHES "^(/defaultlib:)?(msvcrt|libcmt|gcc_s)d?$" OR library STREQUAL "")
            continue()
        endif()
        # Anything else is a plain name, or something has gone wrong with how the list was read.
        if(NOT library MATCHES "^[-_.a-zA-Z0-9]+$")
            message(FATAL_ERROR "rustc's list of libraries has something other than a library in it, ${library}: ${list}")
        endif()
        list(APPEND result_libraries "${library}")
    endforeach()
    set(${out_libraries} "${result_libraries}" PARENT_SCOPE)
endfunction()
