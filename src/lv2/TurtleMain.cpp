// Usage: <program> <bundle folder> <binary's file name>
//
// Writes an LV2 bundle's manifest.ttl and <name>.ttl from the plug-in's ParamSpec table and PluginInfo, so that the two
// descriptions of a parameter can't drift apart. tools/lv2compare reads the result, and checks it against the VST3.

#include "hadronyche/lv2/PluginInfo.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

using namespace Hadronyche;
using namespace Hadronyche::Lv2;

namespace {

// As few decimals as give the same value back, because 0.001 is how a person wrote it.
std::string number (double value)
{
    char text[400];
    for (int decimals = 1; decimals <= 20; ++decimals)
    {
        std::snprintf (text, sizeof (text), "%.*f", decimals, value);
        if (std::strtod (text, nullptr) == value)
            break;
    }
    return text;
}

const char* lv2Unit (const char* units)
{
    static const std::map<std::string, const char*> known {
        { "ms", "units:ms" }, { "s", "units:s" }, { "Hz", "units:hz" }, { "%", "units:pc" }, { "dB", "units:db" },
        { "oct", "units:oct" }, { "st", "units:semitone12TET" }, { "ct", "units:cent" }, { "bars", "units:bar" },
    };
    const auto found = known.find (units);
    if (found != known.end ())
        return found->second;
    if (units[0] != 0)
    {
        std::fprintf (stderr, "No LV2 unit for \"%s\"\n", units);
        std::exit (1);
    }
    return nullptr;
}

void writeSimplePort (std::ofstream& out, const char* types, uint32_t index, const char* symbol, const char* name)
{
    out << "    lv2:port [\n        a " << types << " ;\n        lv2:index " << index << " ;\n        lv2:symbol \"" << symbol
        << "\" ;\n        lv2:name \"" << name << "\" ;\n    ] ;\n";
}

void writeControl (std::ofstream& out, const PluginInfo& info, const PortLayout& layout, Steinberg::Vst::ParamID id)
{
    const auto& spec = info.getSpec (id);
    out << "    lv2:port [\n        a lv2:InputPort , lv2:ControlPort ;\n"
        << "        lv2:index " << layout.firstControl + id << " ;\n"
        << "        lv2:symbol \"" << info.symbols[id] << "\" ;\n"
        << "        lv2:name \"" << spec.title << "\" ;\n"
        << "        lv2:minimum " << number (spec.min) << " ;\n"
        << "        lv2:maximum " << number (spec.max) << " ;\n"
        << "        lv2:default " << number (spec.defaultValue) << " ;\n";

    // LV2 has no curve for Curve::Squared, so a host's own slider for one is linear, unlike the panel's knob.
    if (spec.curve == Curve::Log)
        out << "        lv2:portProperty pprop:logarithmic ;\n";
    else if (spec.curve == Curve::Toggle)
        out << "        lv2:portProperty lv2:toggled ;\n";
    else if (spec.curve == Curve::Integer)
        out << "        lv2:portProperty lv2:integer ;\n";
    else if (spec.curve == Curve::List)
        out << "        lv2:portProperty lv2:integer , lv2:enumeration ;\n";

    if (!spec.isAutomatable)
        out << "        lv2:portProperty pprop:notAutomatic ;\n";

    if (spec.isSilentAtMin)
        out << "        lv2:scalePoint [ rdfs:label \"-inf\" ; rdf:value " << number (spec.min) << " ] ;\n";

    for (int i = 0; spec.curve == Curve::List && i < spec.labelCount; ++i)
        out << "        lv2:scalePoint [ rdfs:label \"" << spec.labels[i] << "\" ; rdf:value " << i << " ] ;\n";

    if (const char* unit = lv2Unit (spec.units))
        out << "        units:unit " << unit << " ;\n";

    out << "    ] ;\n";
}

} // namespace

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "Usage: <program> <bundle folder> <binary's file name>\n");
        return 2;
    }

    const auto& info = getPluginInfo ();
    const auto layout = getPortLayout (info);
    const std::string bundle = argv[1];
    const std::string dataFile = std::string (info.name) + ".ttl";

    std::ofstream manifest (bundle + "/manifest.ttl");
    manifest << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
             << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
             << "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n\n"
             << "<" << info.uri << ">\n    a lv2:Plugin ;\n    lv2:binary <" << argv[2] << "> ;\n    rdfs:seeAlso <" << dataFile << "> .\n\n"
             << "<" << uiUri (info) << ">\n    a ui:X11UI ;\n    ui:binary <" << argv[2] << "> ;\n    rdfs:seeAlso <" << dataFile << "> .\n";

    std::ofstream out (bundle + "/" + dataFile);
    out << "# Generated from the plug-in's parameter table and src/lv2/PluginInfo.cpp.\n\n"
        << "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n"
        << "@prefix bufsz: <http://lv2plug.in/ns/ext/buf-size#> .\n"
        << "@prefix doap:  <http://usefulinc.com/ns/doap#> .\n"
        << "@prefix foaf:  <http://xmlns.com/foaf/0.1/> .\n"
        << "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n"
        << "@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .\n"
        << "@prefix opts:  <http://lv2plug.in/ns/ext/options#> .\n"
        << "@prefix pprop: <http://lv2plug.in/ns/ext/port-props#> .\n"
        << "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
        << "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n";
    if (info.hasMessagePorts)
        out << "@prefix rsz:   <http://lv2plug.in/ns/ext/resize-port#> .\n";
    out << "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
        << "@prefix time:  <http://lv2plug.in/ns/ext/time#> .\n"
        << "@prefix ui:    <http://lv2plug.in/ns/extensions/ui#> .\n"
        << "@prefix units: <http://lv2plug.in/ns/extensions/units#> .\n"
        << "@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .\n\n"
        << "<" << info.uri << ">\n"
        << "    a lv2:Plugin , lv2:" << info.lv2Class << " , doap:Project ;\n"
        << "    doap:name \"" << info.name << "\" ;\n"
        << "    doap:license <https://opensource.org/licenses/MIT> ;\n"
        << "    doap:maintainer [ foaf:name \"Hadronyche\" ] ;\n"
        << "    lv2:minorVersion " << info.minorVersion << " ;\n"
        << "    lv2:microVersion " << info.microVersion << " ;\n"
        << "    lv2:requiredFeature urid:map , opts:options , bufsz:boundedBlockLength ;\n"
        << "    lv2:optionalFeature lv2:hardRTCapable ;\n"
        << "    lv2:extensionData state:interface ;\n"
        << "    opts:requiredOption bufsz:maxBlockLength ;\n"
        << "    ui:ui <" << uiUri (info) << "> ;\n";

    if (info.hasAudioInput)
    {
        writeSimplePort (out, "lv2:InputPort , lv2:AudioPort", layout.audioInput, "in_left", "Left in");
        writeSimplePort (out, "lv2:InputPort , lv2:AudioPort", layout.audioInput + 1, "in_right", "Right in");
    }
    writeSimplePort (out, "lv2:OutputPort , lv2:AudioPort", layout.audioOutput, "out_left", "Left");
    writeSimplePort (out, "lv2:OutputPort , lv2:AudioPort", layout.audioOutput + 1, "out_right", "Right");

    if (layout.events != PortLayout::kNone)
    {
        out << "    lv2:port [\n        a lv2:InputPort , atom:AtomPort ;\n        atom:bufferType atom:Sequence ;\n";
        if (info.hasMidiInput)
            out << "        atom:supports midi:MidiEvent ;\n";
        if (info.followsTransport)
            out << "        atom:supports time:Position ;\n";
        // REAPER sends a UI's messages to the port marked as the control port, and the transport to any port that takes
        // it, so with message ports, the mark goes to ui_in.
        if (!info.hasMessagePorts)
            out << "        lv2:designation lv2:control ;\n";
        out << "        lv2:index " << layout.events << " ;\n"
            << "        lv2:symbol \"events\" ;\n"
            << "        lv2:name \"" << (info.hasMidiInput ? "MIDI In" : "Transport") << "\" ;\n    ] ;\n";
    }

    if (layout.latency != PortLayout::kNone)
    {
        out << "    lv2:port [\n        a lv2:OutputPort , lv2:ControlPort ;\n"
            << "        lv2:designation lv2:latency ;\n"
            << "        lv2:portProperty lv2:reportsLatency , lv2:integer , pprop:notOnGUI ;\n"
            << "        lv2:index " << layout.latency << " ;\n"
            << "        lv2:symbol \"latency\" ;\n"
            << "        lv2:name \"Latency\" ;\n"
            << "        lv2:minimum 0 ;\n"
            << "        lv2:maximum 1000000 ;\n"
            << "        units:unit units:frame ;\n    ] ;\n";
    }

    if (info.hasMessagePorts)
    {
        out << "    lv2:port [\n        a lv2:InputPort , atom:AtomPort ;\n        atom:bufferType atom:Sequence ;\n"
            << "        atom:supports atom:Object ;\n"
            << "        lv2:designation lv2:control ;\n"
            << "        lv2:index " << layout.messageInput << " ;\n"
            << "        lv2:symbol \"ui_in\" ;\n"
            << "        lv2:name \"From the panel\" ;\n    ] ;\n"
            << "    lv2:port [\n        a lv2:OutputPort , atom:AtomPort ;\n        atom:bufferType atom:Sequence ;\n"
            << "        atom:supports atom:Object ;\n"
            << "        rsz:minimumSize 65536 ;\n"
            << "        lv2:index " << layout.messageOutput << " ;\n"
            << "        lv2:symbol \"ui_out\" ;\n"
            << "        lv2:name \"To the panel\" ;\n    ] ;\n";
    }

    for (Steinberg::Vst::ParamID id = 0; id < info.paramCount; ++id)
        writeControl (out, info, layout, id);

    out << "    .\n\n"
        << "<" << uiUri (info) << ">\n"
        << "    a ui:X11UI ;\n"
        << "    ui:binary <" << argv[2] << "> ;\n"
        << "    lv2:requiredFeature ui:parent ;\n"
        << "    lv2:optionalFeature ui:idleInterface , ui:noUserResize , ui:resize , ui:touch" << (info.hasMessagePorts ? " , urid:map" : "") << " ;\n";
    if (info.hasMessagePorts)
        out << "    ui:portNotification [\n        ui:plugin <" << info.uri << "> ;\n        lv2:symbol \"ui_out\" ;\n"
            << "        ui:protocol atom:eventTransfer ;\n    ] ;\n";
    out << "    lv2:extensionData ui:idleInterface .\n";
    return out.good () && manifest.good () ? 0 : 1;
}
