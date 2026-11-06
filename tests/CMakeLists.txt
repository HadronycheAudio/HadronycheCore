# The expected values here are written out by hand on purpose, like the plug-ins' compatibility tests: a failure
# means that a change would alter saved projects or the sound of every plug-in using this.

add_executable(HadronycheCoreTests CoreTests.cpp Check.h)
target_link_libraries(HadronycheCoreTests PRIVATE hadronyche_dsp hadronyche_params)
add_test(NAME core COMMAND HadronycheCoreTests)

if(VST3_SDK_DIR)
    option(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES "Build the VST3 SDK example plug-ins" OFF)
    option(SMTG_ENABLE_VST3_HOSTING_EXAMPLES "Build the VST3 SDK example hosts" OFF)
    option(SMTG_ENABLE_VSTGUI_SUPPORT "Build VSTGUI, used for the editor UI" ON)
    option(SMTG_CREATE_PLUGIN_LINK "Symlink the built plug-in into the VST3 folder" OFF)
    add_subdirectory("${VST3_SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    smtg_enable_vst3_sdk()

    add_executable(HadronycheCoreVstTests VstTests.cpp Check.h "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp")
    target_compile_definitions(HadronycheCoreVstTests PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(HadronycheCoreVstTests PRIVATE hadronyche_vst)
    add_test(NAME core-vst COMMAND HadronycheCoreVstTests)

    # Only built. Driving a real panel takes a real plug-in, so the plug-ins' panel tests cover what it does.
    add_library(HadronycheCoreGuiCheck STATIC GuiCheck.cpp)
    target_compile_definitions(HadronycheCoreGuiCheck PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(HadronycheCoreGuiCheck PRIVATE hadronyche_gui)

    # The same goes for the test host, which the plug-ins' test programs are built on.
    add_library(HadronycheCoreHostCheck STATIC HostCheck.cpp)
    target_link_libraries(HadronycheCoreHostCheck PRIVATE hadronyche_testhost)
endif()
