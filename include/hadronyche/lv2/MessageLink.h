#pragma once

#include "pluginterfaces/vst/ivstmessage.h"

#include <memory>

namespace Hadronyche {
namespace Lv2 {

// A plug-in with hasMessagePorts has the VST3 messages between its processor and its controller carried through the
// ports ui_in and ui_out, and so possibly between processes. These let the plug-in's own LV2 code, in the sources it
// gives hadronyche_add_lv2 as LINKS, change what passes, without changing its VST3 classes.

class MessageSender
{
public:
    virtual ~MessageSender () = default;

    // Copies the message. Messages that don't fit in the queue are dropped, so send state rather than changes.
    virtual void send (Steinberg::Vst::IMessage* message) = 0;
};

// The audio side's part. The instance calls it from one thread at a time, and never from the audio thread.
class DspLink
{
public:
    virtual ~DspLink () = default;

    // A message the processor sent its controller. True keeps it from the panel.
    virtual bool fromProcessor (Steinberg::Vst::IMessage*) { return false; }

    // A message from the panel. True keeps it from the processor.
    virtual bool fromPanel (Steinberg::Vst::IMessage*, MessageSender& /* toPanel */) { return false; }

    // About 30 times a second, from the instance's message thread.
    virtual void tick (MessageSender& /* toPanel */) {}
};

// The panel's part, called from the host's UI thread.
class UiLink
{
public:
    virtual ~UiLink () = default;

    // Once the controller exists, before its panel opens.
    virtual void start (MessageSender& /* toProcessor */) {}

    // A message from the audio side. True keeps it from the controller, which this can tell things itself.
    virtual bool fromDsp (Steinberg::Vst::IMessage*, Steinberg::Vst::IConnectionPoint& /* controller */) { return false; }
};

// Defined by the plug-in's LINKS, or by src/lv2/NoLinks.cpp for a plug-in without any, as null.
std::unique_ptr<DspLink> createDspLink ();
std::unique_ptr<UiLink> createUiLink ();

} // namespace Lv2
} // namespace Hadronyche
