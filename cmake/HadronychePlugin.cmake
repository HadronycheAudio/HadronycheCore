# What every Hadronyche plug-in's top-level CMakeLists.txt has in common. A plug-in includes this, and then:
#
#   hadronyche_use_vst3_sdk ()
#   hadronyche_add_plugin (MyPlugin AU_TYPE aufx AU_SUBTYPE MyPl SOURCES src/PluginEntry.cpp ...)
#
# On macOS, a plug-in also has to set CMAKE_OSX_DEPLOYMENT_TARGET before its project (), or it only loads on macOS as
# new as the build machine.

include("${CMAKE_CURRENT_LIST_DIR}/HadronycheRust.cmake")

# A macro rather than a function, because the SDK's flags and targets have to reach the plug-in's own scope.
macro(hadronyche_use_vst3_sdk)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk")

    if(NOT EXISTS "${VST3_SDK_DIR}/CMakeLists.txt" OR NOT EXISTS "${VST3_SDK_DIR}/vstgui4/vstgui/CMakeLists.txt")
        message(FATAL_ERROR "SDK submodule is missing. Run: git submodule update --init --recursive")
    endif()

    if(APPLE)
        if(NOT XCODE)
            message(FATAL_ERROR "On macOS, configure with -G Xcode, which the SDK's Audio Unit wrapper needs")
        endif()

        # Apple's base classes for the SDK's Audio Unit wrapper. 1.1.0 is the first with its headers where the wrapper
        # looks for them, and later ones need newer C++ than the wrapper is built with.
        include(FetchContent)
        FetchContent_Declare(audiounitsdk
            GIT_REPOSITORY https://github.com/apple/AudioUnitSDK.git
            GIT_TAG AudioUnitSDK-1.1.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(audiounitsdk)
        set(SMTG_AUDIOUNIT_SDK_PATH "${audiounitsdk_SOURCE_DIR}" CACHE PATH "Apple's AudioUnitSDK")

        # Not signed with an Apple account: these are signed for the machine they run on, and the installer does the
        # same for what it ships. Timestamps need an account.
        set(SMTG_CODE_SIGN_IDENTITY_MAC "-" CACHE STRING "macOS Code Sign Identity")
        set(SMTG_XCODE_OTHER_CODE_SIGNING_FLAGS "" CACHE STRING "Other code signing flags [Xcode]")

        # The wrapper's build step copies the Audio Unit here, and fails if it's missing.
        file(MAKE_DIRECTORY "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
    endif()

    # These are declared before the SDK so they stay overridable with -D on the command line.
    option(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES "Build the VST3 SDK example plug-ins" OFF)
    option(SMTG_ENABLE_VST3_HOSTING_EXAMPLES "Build the VST3 SDK example hosts" OFF)
    option(SMTG_ENABLE_VSTGUI_SUPPORT "Build VSTGUI, used for the editor UI" ON)
    # Linking into the system VST3 folder needs admin rights or Windows Developer Mode.
    option(SMTG_CREATE_PLUGIN_LINK "Symlink the built plug-in into the VST3 folder" OFF)
    # Linking the C++ runtime statically means users don't need to install Microsoft's runtime separately. Without it,
    # a plug-in fails to load on PCs without that runtime, and neither the tests nor CI would notice.
    option(SMTG_USE_STATIC_CRT "Link the C++ runtime statically" ON)

    add_subdirectory("${VST3_SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    smtg_enable_vst3_sdk()

    # The SDK builds with -ffast-math -ffp-contract=fast on Xcode, which these plug-ins can't work with:
    # - The compiler may assume that NaN and infinity never happen, and remove every std::isfinite. Those checks are
    #   what keep a broken sample out of the filters and saved projects.
    # - On Apple Silicon, fused multiplies keep residues that should cancel. 2 * (1 - 100 / 100) comes out at about
    #   -4e-17, so that a fully wet plug-in still lets through a trace of its dry signal. Exact gains and lossless
    #   loops depend on arithmetic like this.
    # GCC also contracts multiplies by default wherever the target has fused instructions, so Linux needs this too.
    if(XCODE OR LINUX)
        add_compile_options(-fno-fast-math -ffp-contract=off)
    endif()

    # Linking the C++ runtime statically means a plug-in built on one distribution doesn't need that distribution's
    # newer runtime on another. As with SMTG_USE_STATIC_CRT, nothing would notice on the machine that built it.
    if(LINUX)
        add_link_options(-static-libstdc++ -static-libgcc)
    endif()

    # After the SDK, so that the library builds with the same flags and runtime as the plug-in.
    add_subdirectory("${HADRONYCHE_CORE_DIR}" "${CMAKE_BINARY_DIR}/hadronychecore")
endmacro()

# An Audio Unit for a plug-in, using the SDK's wrapper around its VST3. AU_TYPE is aufx for an effect and aumu for an
# instrument. The manufacturer and AU_SUBTYPE codes are how hosts find it in saved projects, so never change them.
function(hadronyche_add_audio_unit name au_type au_subtype)
    set(HADRONYCHE_AU_NAME "${name}")
    set(HADRONYCHE_AU_TYPE "${au_type}")
    set(HADRONYCHE_AU_SUBTYPE "${au_subtype}")
    set(HADRONYCHE_AU_MANUFACTURER "Hdrn")
    math(EXPR HADRONYCHE_AU_VERSION "(${PROJECT_VERSION_MAJOR} << 16) + (${PROJECT_VERSION_MINOR} << 8) + ${PROJECT_VERSION_PATCH}")
    set(HADRONYCHE_AU_VERSION_STRING "${PROJECT_VERSION}")
    if(au_type STREQUAL "aumu")
        set(HADRONYCHE_AU_INPUTS 0)
    else()
        set(HADRONYCHE_AU_INPUTS 2)
    endif()
    configure_file("${HADRONYCHE_CORE_DIR}/cmake/au-info.plist.in" "${CMAKE_BINARY_DIR}/au-info.plist" @ONLY)

    list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_DIR}/cmake/modules")
    if(NOT public_sdk_SOURCE_DIR)
        set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    endif()
    include(SMTG_AddVST3AuV2)

    string(TOLOWER "${name}" lower_name)
    smtg_target_add_auv2(${name}-au
        BUNDLE_NAME ${name}
        BUNDLE_IDENTIFIER com.hadronyche.audiounit.${lower_name}
        INFO_PLIST_TEMPLATE "${CMAKE_BINARY_DIR}/au-info.plist"
        VST3_PLUGIN_TARGET ${name}
    )
endfunction()

# The VST3 plug-in target, with its version available as <NAME>_VERSION, and the library's code linked in, and on
# macOS an Audio Unit target called <name>-au. With a <name>-rust target from hadronyche_add_rust, that's linked in too.
function(hadronyche_add_plugin name)
    cmake_parse_arguments(PARSE_ARGV 1 PLUGIN "" "AU_TYPE;AU_SUBTYPE" "SOURCES")

    smtg_add_vst3plugin(${name}
        PACKAGE_NAME "${name}"
        SOURCES_LIST ${PLUGIN_SOURCES}
    )

    string(TOUPPER "${name}" upper_name)
    target_include_directories(${name} PRIVATE src)
    target_compile_definitions(${name} PRIVATE
        ${upper_name}_VERSION="${PROJECT_VERSION}"
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
    )
    target_link_libraries(${name} PRIVATE sdk vstgui_support hadronyche_dsp hadronyche_vst hadronyche_gui)
    if(TARGET ${name}-rust)
        target_link_libraries(${name} PRIVATE ${name}-rust)
    endif()

    # Only the entry points are for a host, and anything else exported could clash with it or with other plug-ins.
    # -z nodelete keeps the plug-in loaded after a host closes it, because Pango's font thread keeps running, and would
    # crash in an unloaded library.
    if(LINUX)
        target_link_options(${name} PRIVATE
            "-Wl,--version-script=${HADRONYCHE_CORE_DIR}/cmake/linux-exports.map"
            -Wl,-z,nodelete
        )
    endif()

    if(APPLE)
        string(TOLOWER "${name}" lower_name)
        smtg_target_set_bundle(${name}
            BUNDLE_IDENTIFIER "com.hadronyche.vst3.${lower_name}"
            COMPANY_NAME "Hadronyche"
        )
        hadronyche_add_audio_unit(${name} ${PLUGIN_AU_TYPE} ${PLUGIN_AU_SUBTYPE})
    endif()
endfunction()

# An LV2 version of a plug-in on Linux, which is the target <name>-lv2, building a bundle in <build>/LV2/<name>.lv2. It
# needs lv2-dev. It's built from the VST3 target's own sources under src/, so call it after hadronyche_add_plugin, and
# the plug-in also has to have src/lv2/PluginInfo.cpp. SOURCES are any more it needs. LINKS are a plug-in's own sources
# for its message ports, which define createDspLink and createUiLink, as include/hadronyche/lv2/MessageLink.h describes.
# Elsewhere this does nothing.
function(hadronyche_add_lv2 name)
    if(NOT LINUX)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 LV2 "" "" "SOURCES;LINKS")
    if(NOT LV2_LINKS)
        set(LV2_LINKS "${HADRONYCHE_CORE_DIR}/src/lv2/NoLinks.cpp")
    endif()

    # The SDK adds its own entry point to the VST3's sources, and the LV2 gets its own below.
    get_target_property(vst3_sources ${name} SOURCES)
    foreach(source ${vst3_sources})
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR "${source}" is_own)
        cmake_path(IS_PREFIX VST3_SDK_DIR "${source}" is_sdk)
        if(is_own AND NOT is_sdk AND source MATCHES "\\.cpp$")
            list(APPEND LV2_SOURCES "${source}")
        endif()
    endforeach()

    set(bundle "${CMAKE_BINARY_DIR}/LV2/${name}.lv2")
    add_library(${name}-lv2 MODULE
        "${HADRONYCHE_CORE_DIR}/src/lv2/Common.cpp"
        "${HADRONYCHE_CORE_DIR}/src/lv2/Lv2Wrapper.cpp"
        "${HADRONYCHE_CORE_DIR}/src/lv2/Message.cpp"
        "${HADRONYCHE_CORE_DIR}/src/lv2/Ui.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
        src/lv2/PluginInfo.cpp
        ${LV2_SOURCES}
        ${LV2_LINKS}
    )
    smtg_target_add_library_main(${name}-lv2)
    string(TOUPPER "${name}" upper_name)
    target_include_directories(${name}-lv2 PRIVATE src)
    target_compile_definitions(${name}-lv2 PRIVATE ${upper_name}_VERSION="${PROJECT_VERSION}")
    target_link_libraries(${name}-lv2 PRIVATE sdk vstgui_support hadronyche_dsp hadronyche_vst hadronyche_gui xcb ${CMAKE_DL_LIBS})
    if(TARGET ${name}-rust)
        target_link_libraries(${name}-lv2 PRIVATE ${name}-rust)
    endif()
    # A shared library can link with symbols missing, and then only crash when a host reaches them. No host provides any.
    # -z nodelete for the same reason as the VST3's.
    target_link_options(${name}-lv2 PRIVATE
        "-Wl,--version-script=${HADRONYCHE_CORE_DIR}/cmake/lv2-exports.map"
        -Wl,--no-undefined
        -Wl,-z,nodelete
        "$<$<CONFIG:Release>:-s>"
    )
    set_target_properties(${name}-lv2 PROPERTIES
        OUTPUT_NAME ${name}
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${bundle}"
    )

    # The parameter table comes from src/Parameters.cpp, or from the Rust side when the plug-in has one.
    add_executable(${name}-lv2-turtle
        "${HADRONYCHE_CORE_DIR}/src/lv2/TurtleMain.cpp"
        src/lv2/PluginInfo.cpp
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Parameters.cpp")
        target_sources(${name}-lv2-turtle PRIVATE src/Parameters.cpp)
    endif()
    target_include_directories(${name}-lv2-turtle PRIVATE src)
    target_link_libraries(${name}-lv2-turtle PRIVATE sdk hadronyche_dsp hadronyche_vst)
    if(TARGET ${name}-rust)
        target_link_libraries(${name}-lv2-turtle PRIVATE ${name}-rust)
    endif()
    add_custom_command(TARGET ${name}-lv2 POST_BUILD
        COMMAND ${name}-lv2-turtle "${bundle}" "${name}.so"
        VERBATIM
    )
endfunction()

# Tests that the LV2 from hadronyche_add_lv2 matches the plug-in's VST3 bit for bit, from a bundle where it's built, and
# from one as shallow as a path can get.
function(hadronyche_add_lv2_tests name)
    if(NOT LINUX)
        return()
    endif()

    add_executable(${name}Lv2Compare
        "${HADRONYCHE_CORE_DIR}/tools/lv2compare/main.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
    )
    target_include_directories(${name}Lv2Compare PRIVATE "${VST3_SDK_DIR}")
    target_link_libraries(${name}Lv2Compare PRIVATE ${CMAKE_DL_LIBS})
    # It counts allocations inside the plug-in by exporting malloc to it.
    set_target_properties(${name}Lv2Compare PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(${name}Lv2Compare ${name} ${name}-lv2)

    add_executable(${name}Lv2Panel
        "${HADRONYCHE_CORE_DIR}/tools/lv2panel/main.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
    )
    target_include_directories(${name}Lv2Panel PRIVATE "${VST3_SDK_DIR}")
    target_link_libraries(${name}Lv2Panel PRIVATE xcb ${CMAKE_DL_LIBS})
    # So that a build of the comparison builds everything the LV2's tests need.
    add_dependencies(${name}Lv2Compare ${name}Lv2Panel)

    set(bundle "${CMAKE_BINARY_DIR}/LV2/${name}.lv2")
    add_test(NAME lv2-matches-vst3 COMMAND ${name}Lv2Compare "$<TARGET_FILE:${name}>" "${bundle}")
    set_tests_properties(lv2-matches-vst3 PROPERTIES TIMEOUT 600)

    add_test(NAME lv2-shallow-path COMMAND sh -c
        "d=$(mktemp -d /tmp/${name}-XXXXXX.lv2) && cp -r \"$2\"/. \"$d/\" && \"$0\" \"$1\" \"$d\" load-only; s=$?; rm -rf \"$d\"; exit $s"
        "$<TARGET_FILE:${name}Lv2Compare>" "$<TARGET_FILE:${name}>" "${bundle}")

    # With its own screen, because two panels have to fit side by side, and from a bundle at the shallowest path there is,
    # which is the one that VSTGUI can't handle if it's given the binary's handle.
    add_test(NAME lv2-panel COMMAND sh -c
        "d=$(mktemp -d /tmp/${name}-XXXXXX.lv2) && cp -r \"$2\"/. \"$d/\" && xvfb-run -a -s '-screen 0 4000x1200x24' \"$0\" \"$1\" \"$d\"; s=$?; rm -rf \"$d\"; exit $s"
        "$<TARGET_FILE:${name}Lv2Panel>" "$<TARGET_FILE:${name}>" "${bundle}")
    set_tests_properties(lv2-panel PROPERTIES TIMEOUT 300)
endfunction()
