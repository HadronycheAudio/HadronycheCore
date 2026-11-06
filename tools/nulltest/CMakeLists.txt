cmake_minimum_required(VERSION 3.25)

# A standalone program, so that the same binary dumps both sides of a comparison, whatever state the plug-ins
# and the library are in. It uses only the SDK's interfaces, and needs no built SDK.
project(NullTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VST3_SDK_DIR "" CACHE PATH "A VST3 SDK checkout, such as a plug-in's external/vst3sdk")
if(NOT EXISTS "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp")
    message(FATAL_ERROR "Set VST3_SDK_DIR to a VST3 SDK checkout")
endif()

add_executable(NullTest
    main.cpp
    "${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp"
    "${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp"
    "${VST3_SDK_DIR}/pluginterfaces/base/ustring.cpp"
    "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp"
)
target_include_directories(NullTest PRIVATE "${VST3_SDK_DIR}")
target_compile_definitions(NullTest PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    # wmain, and the SDK needs its headers read as UTF-8.
    target_compile_options(NullTest PRIVATE /utf-8)
endif()
target_link_libraries(NullTest PRIVATE ${CMAKE_DL_LIBS})
