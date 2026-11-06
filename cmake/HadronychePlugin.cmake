# What every Hadronyche plug-in's top-level CMakeLists.txt has in common. A plug-in includes this, and then:
#
#   hadronyche_use_vst3_sdk ()
#   hadronyche_add_plugin (MyPlugin SOURCES src/PluginEntry.cpp ...)

# A macro rather than a function, because the SDK's flags and targets have to reach the plug-in's own scope.
macro(hadronyche_use_vst3_sdk)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk")

    if(NOT EXISTS "${VST3_SDK_DIR}/CMakeLists.txt" OR NOT EXISTS "${VST3_SDK_DIR}/vstgui4/vstgui/CMakeLists.txt")
        message(FATAL_ERROR "SDK submodule is missing. Run: git submodule update --init --recursive")
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

    # After the SDK, so that the library builds with the same flags and runtime as the plug-in.
    add_subdirectory("${HADRONYCHE_CORE_DIR}" "${CMAKE_BINARY_DIR}/hadronychecore")
endmacro()

# The VST3 plug-in target, with its version available as <NAME>_VERSION, and the library's code linked in.
function(hadronyche_add_plugin name)
    cmake_parse_arguments(PARSE_ARGV 1 PLUGIN "" "" "SOURCES")

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
endfunction()
