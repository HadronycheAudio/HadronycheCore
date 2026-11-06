cmake_minimum_required(VERSION 3.25)

project(HadronycheCore VERSION 0.1.0 LANGUAGES CXX)

# A plug-in adds this after smtg_enable_vst3_sdk (), so that everything here builds with the same flags as the
# plug-in, and it provides the SDK's targets. The SDK isn't a submodule here, which would clone it twice.

# Everything is in one include folder. These targets say what each part needs.
add_library(hadronyche_dsp INTERFACE)
target_include_directories(hadronyche_dsp INTERFACE include)

# Doesn't need the SDK, because a plug-in's tests use its parameter headers without linking it.
add_library(hadronyche_params INTERFACE)
target_include_directories(hadronyche_params INTERFACE include)

add_library(hadronyche_vst INTERFACE)
target_link_libraries(hadronyche_vst INTERFACE hadronyche_params sdk)

# Its source is compiled as part of the plug-in, with the same flags and runtime.
add_library(hadronyche_gui INTERFACE)
target_sources(hadronyche_gui INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/PanelEditor.cpp")
target_link_libraries(hadronyche_gui INTERFACE hadronyche_vst vstgui_support)

if(PROJECT_IS_TOP_LEVEL)
    # The parts using the SDK need a checkout of it, at the same version as the plug-ins. A plug-in sets this itself.
    set(VST3_SDK_DIR "" CACHE PATH "A VST3 SDK checkout, to also test the parts that use it")
endif()

# For test programs, which load a built plug-in and link none of it, or of the SDK beyond these few files.
if(VST3_SDK_DIR)
    add_library(hadronyche_testhost INTERFACE)
    target_sources(hadronyche_testhost INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/testhost/Host.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
        "${VST3_SDK_DIR}/pluginterfaces/base/ustring.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
    )
    target_include_directories(hadronyche_testhost INTERFACE "${VST3_SDK_DIR}")
    target_compile_definitions(hadronyche_testhost INTERFACE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(hadronyche_testhost INTERFACE hadronyche_params)
    if(WIN32)
        target_link_libraries(hadronyche_testhost INTERFACE user32 gdi32)
    elseif(APPLE)
        target_link_libraries(hadronyche_testhost INTERFACE "-framework CoreFoundation")
    endif()
endif()

# One CTest test per test in the program, so that a single one can be run with ctest -R. The panel test drives a real
# window with Windows' mouse messages, so it only exists there.
function(hadronyche_add_host_tests program plugin)
    set(test_names ${ARGN})
    if(NOT WIN32)
        list(REMOVE_ITEM test_names panel)
    endif()
    # On macOS, CMake calls the bundle .bundle, though the SDK has Xcode build it as .vst3.
    if(APPLE)
        set(binary "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/${plugin}.vst3/Contents/MacOS/${plugin}")
    else()
        set(binary "$<TARGET_FILE:${plugin}>")
    endif()
    foreach(test_name ${test_names})
        add_test(NAME ${test_name} COMMAND ${program} "${binary}" ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
    endforeach()
endfunction()

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    enable_testing()
    add_subdirectory(tests)
endif()
