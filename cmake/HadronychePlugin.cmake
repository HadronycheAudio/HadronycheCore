# What every Hadronyche plug-in's top-level CMakeLists.txt has in common. A plug-in includes this, and then:
#
#   hadronyche_use_vst3_sdk ()
#   hadronyche_add_plugin (MyPlugin AU_TYPE aufx AU_SUBTYPE MyPl SOURCES src/PluginEntry.cpp ...)
#
# On macOS, a plug-in also has to set CMAKE_OSX_DEPLOYMENT_TARGET before its project (), or it only loads on macOS as
# new as the build machine.

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
    if(XCODE)
        add_compile_options(-fno-fast-math -ffp-contract=off)
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
# macOS an Audio Unit target called <name>-au.
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

    if(APPLE)
        string(TOLOWER "${name}" lower_name)
        smtg_target_set_bundle(${name}
            BUNDLE_IDENTIFIER "com.hadronyche.vst3.${lower_name}"
            COMPANY_NAME "Hadronyche"
        )
        hadronyche_add_audio_unit(${name} ${PLUGIN_AU_TYPE} ${PLUGIN_AU_SUBTYPE})
    endif()
endfunction()
