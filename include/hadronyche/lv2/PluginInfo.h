#pragma once

#include "hadronyche/vst/ParamSpec.h"

#include "pluginterfaces/vst/vsttypes.h"

#include <cstdint>
#include <string>

namespace Hadronyche {
namespace Lv2 {

// What a plug-in tells the LV2 wrapper and the Turtle generator about itself, in its own src/lv2/PluginInfo.cpp.
//
// The URI and the symbols are saved in hosts' projects, and the flags decide the port numbers, so all of these are
// permanent from a plug-in's first LV2 release. New parameters add a symbol at the end.
struct PluginInfo
{
    const char* uri;
    const char* name;
    const char* lv2Class; // A class from LV2's core, such as "InstrumentPlugin".
    const ParamSpec& (*getSpec) (Steinberg::Vst::ParamID);
    uint32_t paramCount;
    const char* const* symbols; // One for each parameter, in ParamId order.
    int minorVersion;           // LV2 treats an odd one as a development build, so this isn't the plug-in's version.
    int microVersion;
    bool hasAudioInput;
    bool hasMidiInput;
    bool followsTransport;
    bool reportsLatency;

    // The ports ui_in and ui_out, which carry VST3 messages between the processor and the panel. See MessageLink.h.
    bool hasMessagePorts { false };
};

// Defined in the plug-in.
const PluginInfo& getPluginInfo ();

// The panel's URI, which is saved in hosts' projects like the plug-in's.
inline std::string uiUri (const PluginInfo& info)
{
    return std::string (info.uri) + "#ui";
}

// Audio, then events, latency and the message ports if it has them, then the controls in ParamId order, so that a new
// parameter only adds a port at the end.
struct PortLayout
{
    static constexpr uint32_t kNone = 0xFFFFFFFF;

    uint32_t audioInput { kNone }; // Then the same plus one, for the right channel.
    uint32_t audioOutput { 0 };
    uint32_t events { kNone };
    uint32_t latency { kNone };
    uint32_t messageInput { kNone };
    uint32_t messageOutput { kNone };
    uint32_t firstControl { 0 };
};

inline PortLayout getPortLayout (const PluginInfo& info)
{
    PortLayout layout;
    uint32_t next = 0;
    if (info.hasAudioInput)
    {
        layout.audioInput = next;
        next += 2;
    }
    layout.audioOutput = next;
    next += 2;
    if (info.hasMidiInput || info.followsTransport)
        layout.events = next++;
    if (info.reportsLatency)
        layout.latency = next++;
    if (info.hasMessagePorts)
    {
        layout.messageInput = next++;
        layout.messageOutput = next++;
    }
    layout.firstControl = next;
    return layout;
}

} // namespace Lv2
} // namespace Hadronyche
