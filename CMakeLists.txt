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
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    enable_testing()
    add_subdirectory(tests)
endif()
