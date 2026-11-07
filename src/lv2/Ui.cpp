// Shows a plug-in's own VST3 panel as an LV2 UI, through its own controller. The controller hears about the audio side
// through control ports, and with message ports through the VST3 messages that Lv2Wrapper.cpp carries, never through
// shared memory, since a host can run the UI in another process. tools/lv2panel compares what this draws and writes
// with the VST3's panel.

#include "Common.h"
#include "MessageAtoms.h"

#include "hadronyche/lv2/Message.h"
#include "hadronyche/lv2/MessageLink.h"
#include "hadronyche/lv2/PluginInfo.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <xcb/xcb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using namespace Steinberg;

namespace Hadronyche {
namespace Lv2 {

namespace {

// VSTGUI has one run loop per process, which every panel uses until the last one closes, and a UI can close while
// another stays open. So the loop isn't any one UI's, and each UI's idle call services it. A host calls those from one
// thread, so this needs no lock.
class RunLoop : public Linux::IRunLoop
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Linux::IRunLoop::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<Linux::IRunLoop*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
    {
        _events.push_back ({ handler, fd });
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* handler) override
    {
        _events.erase (std::remove_if (_events.begin (), _events.end (), [handler] (const EventEntry& entry) { return entry.handler == handler; }), _events.end ());
        return kResultTrue;
    }

    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* handler, Linux::TimerInterval milliseconds) override
    {
        const auto interval = std::chrono::milliseconds (milliseconds);
        _timers.push_back ({ handler, interval, Clock::now () + interval });
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* handler) override
    {
        _timers.erase (std::remove_if (_timers.begin (), _timers.end (), [handler] (const TimerEntry& entry) { return entry.handler == handler; }), _timers.end ());
        return kResultTrue;
    }

    // Each handler is looked up again before it's called, because one can unregister another as it runs, and VSTGUI
    // frees a handler when it does.
    void service ()
    {
        // Without polling its file first, because xcb reads ahead: events can be waiting in its queue with nothing left
        // to read from the file. VSTGUI's handler doesn't block.
        for (const auto& entry : std::vector<EventEntry> (_events))
        {
            const auto isSame = [&entry] (const EventEntry& other) { return other.handler == entry.handler; };
            if (std::any_of (_events.begin (), _events.end (), isSame))
                entry.handler->onFDIsSet (entry.fd);
        }

        const auto now = Clock::now ();
        for (const auto& entry : std::vector<TimerEntry> (_timers))
        {
            const auto found = std::find_if (_timers.begin (), _timers.end (), [&entry] (const TimerEntry& other) { return other.handler == entry.handler; });
            if (found == _timers.end () || found->next > now)
                continue;
            found->next = now + found->interval;
            entry.handler->onTimer ();
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    struct EventEntry
    {
        Linux::IEventHandler* handler;
        Linux::FileDescriptor fd;
    };

    struct TimerEntry
    {
        Linux::ITimerHandler* handler;
        std::chrono::milliseconds interval;
        Clock::time_point next;
    };

    std::vector<EventEntry> _events;
    std::vector<TimerEntry> _timers;
};

RunLoop gRunLoop;

// What PanelEditor asks a host for, and how it finds the run loop.
class PlugFrame : public IPlugFrame
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<IPlugFrame*> (this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual (iid, Linux::IRunLoop::iid))
        {
            *object = static_cast<Linux::IRunLoop*> (&gRunLoop);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    // The panels are a fixed size, so this only happens as one opens.
    tresult PLUGIN_API resizeView (IPlugView* view, ViewRect* size) override { return view->onSize (size); }
};

class Ui : public Vst::IComponentHandler, public Vst::IConnectionPoint, private MessageSender
{
public:
    Ui (const PluginInfo& info, LV2UI_Write_Function write, LV2UI_Controller controller)
    : _info (info), _layout (getPortLayout (info)), _write (write), _hostController (controller)
    {
    }

    ~Ui ()
    {
        if (_view)
        {
            if (_isAttached)
                _view->removed ();
            _view->setFrame (nullptr);
            _view->release ();
        }
        if (_connection)
        {
            if (_window)
                xcb_destroy_window (_connection, _window);
            xcb_disconnect (_connection); // This flushes.
        }
        if (_controllerConnection)
        {
            _controllerConnection->disconnect (this);
            _controllerConnection->release ();
        }
        _link.reset ();
        if (_controller)
        {
            _controller->setComponentHandler (nullptr);
            _controller->terminate ();
            _controller->release ();
        }
    }

    // Without a URID map, the panel opens without messages, like a VST3 panel whose processor never introduced itself.
    bool create (xcb_window_t parent, const LV2UI_Resize* resize, const LV2UI_Touch* touch, LV2_URID_Map* map)
    {
        _touch = touch;
        _portValues.resize (_info.paramCount);
        _isEditing.assign (_info.paramCount, false);
        for (Vst::ParamID id = 0; id < _info.paramCount; ++id)
            _portValues[id] = static_cast<float> (_info.getSpec (id).defaultValue);

        return createController () && (!_info.hasMessagePorts || !map || connectMessages (map)) && createView () && openWindow (parent, resize);
    }

    uint32_t window () const { return _window; }

    void portEvent (uint32_t port, uint32_t size, uint32_t format, const void* buffer)
    {
        if (port == _layout.messageOutput)
        {
            receive (size, format, buffer);
            return;
        }

        const uint32_t index = port - _layout.firstControl;
        if (index >= _info.paramCount || format != 0 || size != sizeof (float))
            return;

        // A host echoes what this wrote, late enough to move a knob back under the mouse, so a control being edited
        // ignores its port.
        const float value = *static_cast<const float*> (buffer);
        if (value == _portValues[index] || _isEditing[index] || !std::isfinite (value))
            return;

        _portValues[index] = value;
        _controller->setParamNormalized (index, controlToNormalized (_info.getSpec (index), value));
    }

    tresult PLUGIN_API beginEdit (Vst::ParamID id) override
    {
        if (id >= _info.paramCount)
            return kInvalidArgument;

        _isEditing[id] = true;
        setTouched (id, true);
        return kResultOk;
    }

    tresult PLUGIN_API performEdit (Vst::ParamID id, Vst::ParamValue normalized) override
    {
        if (id >= _info.paramCount)
            return kInvalidArgument;

        const float value = static_cast<float> (normalizedToPlain (_info.getSpec (id), normalized));
        _portValues[id] = value;
        _write (_hostController, _layout.firstControl + id, sizeof (float), 0, &value);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit (Vst::ParamID id) override
    {
        if (id >= _info.paramCount)
            return kInvalidArgument;

        _isEditing[id] = false;
        setTouched (id, false);
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent (int32) override { return kResultOk; }

    // IConnectionPoint, as the controller's peer, which gets what the controller sends its processor.
    tresult PLUGIN_API connect (Vst::IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API disconnect (Vst::IConnectionPoint*) override { return kResultOk; }

    tresult PLUGIN_API notify (Vst::IMessage* message) override
    {
        if (!message)
            return kInvalidArgument;
        send (message);
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Vst::IComponentHandler::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
        {
            *object = static_cast<Vst::IComponentHandler*> (this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual (iid, Vst::IConnectionPoint::iid))
        {
            *object = static_cast<Vst::IConnectionPoint*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

private:
    bool connectMessages (LV2_URID_Map* map)
    {
        _atoms.map (map, _info);
        _eventTransfer = map->map (map->handle, LV2_ATOM__eventTransfer);
        if (_controller->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&_controllerConnection)) != kResultOk)
            return false;
        _controllerConnection->connect (this);

        _link = createUiLink ();
        if (_link)
            _link->start (*this);
        return true;
    }

    // MessageSender, to the processor. Only messages from the wrapper's own HostContext can be read back into bytes, and
    // a controller's allocateMessage gives those.
    void send (Vst::IMessage* message) override
    {
        auto* readable = dynamic_cast<Message*> (message);
        if (!readable || !_controllerConnection)
            return;

        const auto bytes = readable->toBytes ();
        std::vector<uint8_t> atom (MessageAtoms::atomSize (static_cast<uint32_t> (bytes.size ())));
        _atoms.write (atom.data (), bytes.data (), static_cast<uint32_t> (bytes.size ()));
        _write (_hostController, _layout.messageInput, static_cast<uint32_t> (atom.size ()), _eventTransfer, atom.data ());
    }

    void receive (uint32_t size, uint32_t format, const void* buffer)
    {
        const void* bytes = nullptr;
        uint32_t byteCount = 0;
        if (!_controllerConnection || format != _eventTransfer || !buffer
            || !_atoms.read (static_cast<const LV2_Atom*> (buffer), size, bytes, byteCount))
            return;

        if (auto* message = Message::fromBytes (bytes, byteCount))
        {
            if (!_link || !_link->fromDsp (message, *_controllerConnection))
                _controllerConnection->notify (message);
            message->release ();
        }
    }

    void setTouched (Vst::ParamID id, bool isGrabbed)
    {
        if (_touch)
            _touch->touch (_touch->handle, _layout.firstControl + id, isGrabbed);
    }

    bool createController ()
    {
        auto* factory = GetPluginFactory ();
        if (!factory)
            return false;

        TUID controllerId {};
        bool isFound = false;
        for (int32 i = 0; i < factory->countClasses () && !isFound; ++i)
        {
            PClassInfo classInfo;
            if (factory->getClassInfo (i, &classInfo) == kResultOk && std::strcmp (classInfo.category, kVstComponentControllerClass) == 0)
            {
                std::memcpy (controllerId, classInfo.cid, sizeof (TUID));
                isFound = true;
            }
        }
        const bool isCreated = isFound && factory->createInstance (controllerId, Vst::IEditController::iid, reinterpret_cast<void**> (&_controller)) == kResultOk;
        factory->release ();
        if (!isCreated || _controller->initialize (&_hostContext) != kResultOk)
            return false;

        _controller->setComponentHandler (this);
        return true;
    }

    bool createView ()
    {
        _view = _controller->createView (Vst::ViewType::kEditor);
        if (!_view || _view->isPlatformTypeSupported (kPlatformTypeX11EmbedWindowID) != kResultTrue)
            return false;

        _view->setFrame (&_plugFrame);
        return true;
    }

    // A window of its own to give the host, because VSTGUI keeps the one it makes to itself, and because a host may well
    // pass a parent that holds other things. The connection stays open with it, since a window ends with its connection.
    bool openWindow (xcb_window_t parent, const LV2UI_Resize* resize)
    {
        ViewRect size;
        if (_view->getSize (&size) != kResultTrue)
            return false;

        _connection = xcb_connect (nullptr, nullptr);
        if (xcb_connection_has_error (_connection) != 0)
            return false;

        const auto width = static_cast<uint16_t> (size.getWidth ());
        const auto height = static_cast<uint16_t> (size.getHeight ());
        _window = xcb_generate_id (_connection);
        xcb_create_window (_connection, XCB_COPY_FROM_PARENT, _window, parent, 0, 0, width, height, 0,
                           XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);

        // WM_NORMAL_HINTS, by its numbers from the X protocol, because some hosts size their window from this. The flags
        // say that this gives a minimum, a maximum and a base size.
        std::array<uint32_t, 18> hints {};
        hints[0] = (1u << 4) | (1u << 5) | (1u << 8);
        hints[5] = hints[7] = hints[15] = width;
        hints[6] = hints[8] = hints[16] = height;
        constexpr xcb_atom_t kWmNormalHints = 40;
        constexpr xcb_atom_t kWmSizeHints = 41;
        xcb_change_property (_connection, XCB_PROP_MODE_REPLACE, _window, kWmNormalHints, kWmSizeHints, 32, static_cast<uint32_t> (hints.size ()), hints.data ());
        xcb_map_window (_connection, _window);

        // VSTGUI's own connection is about to name this window, so the server has to have it by then.
        std::free (xcb_get_input_focus_reply (_connection, xcb_get_input_focus (_connection), nullptr));
        if (xcb_connection_has_error (_connection) != 0)
            return false;

        if (_view->attached (reinterpret_cast<void*> (static_cast<uintptr_t> (_window)), kPlatformTypeX11EmbedWindowID) != kResultOk)
            return false;
        _isAttached = true;

        if (resize)
            resize->ui_resize (resize->handle, width, height);
        return true;
    }

    const PluginInfo& _info;
    const PortLayout _layout;
    LV2UI_Write_Function _write;
    LV2UI_Controller _hostController;
    const LV2UI_Touch* _touch { nullptr };

    HostContext _hostContext;
    PlugFrame _plugFrame;
    Vst::IEditController* _controller { nullptr };
    IPlugView* _view { nullptr };
    bool _isAttached { false };
    xcb_connection_t* _connection { nullptr };
    xcb_window_t _window { 0 };

    std::vector<float> _portValues; // What each port last held, as this knows it.
    std::vector<bool> _isEditing;

    MessageAtoms _atoms;
    LV2_URID _eventTransfer { 0 };
    Vst::IConnectionPoint* _controllerConnection { nullptr };
    std::unique_ptr<UiLink> _link;
};

LV2UI_Handle instantiate (const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function write, LV2UI_Controller controller,
                          LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    void* parent = nullptr;
    const LV2UI_Resize* resize = nullptr;
    const LV2UI_Touch* touch = nullptr;
    LV2_URID_Map* map = nullptr;
    for (int i = 0; features && features[i]; ++i)
    {
        if (std::strcmp (features[i]->URI, LV2_UI__parent) == 0)
            parent = features[i]->data;
        else if (std::strcmp (features[i]->URI, LV2_UI__resize) == 0)
            resize = static_cast<const LV2UI_Resize*> (features[i]->data);
        else if (std::strcmp (features[i]->URI, LV2_UI__touch) == 0)
            touch = static_cast<const LV2UI_Touch*> (features[i]->data);
        else if (std::strcmp (features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<LV2_URID_Map*> (features[i]->data);
    }

    // Nothing draws without idle calls, but this can't insist on ui:idleInterface. REAPER calls idle without passing
    // it, and crashes if a UI then refuses to open.
    if (!parent || !write || !enterModule ())
        return nullptr;

    auto* ui = new (std::nothrow) Ui (getPluginInfo (), write, controller);
    if (!ui || !ui->create (static_cast<xcb_window_t> (reinterpret_cast<uintptr_t> (parent)), resize, touch, map))
    {
        delete ui;
        exitModule ();
        return nullptr;
    }

    *widget = reinterpret_cast<LV2UI_Widget> (static_cast<uintptr_t> (ui->window ()));
    return ui;
}

void cleanup (LV2UI_Handle handle)
{
    delete static_cast<Ui*> (handle);
    exitModule ();
}

void portEvent (LV2UI_Handle handle, uint32_t port, uint32_t size, uint32_t format, const void* buffer)
{
    static_cast<Ui*> (handle)->portEvent (port, size, format, buffer);
}

int idle (LV2UI_Handle)
{
    gRunLoop.service ();
    return 0;
}

const void* extensionData (const char* uri)
{
    static const LV2UI_Idle_Interface idleInterface { idle };
    return std::strcmp (uri, LV2_UI__idleInterface) == 0 ? &idleInterface : nullptr;
}

} // namespace

} // namespace Lv2
} // namespace Hadronyche

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor (uint32_t index)
{
    using namespace Hadronyche::Lv2;
    static const std::string uri = uiUri (getPluginInfo ());
    static const LV2UI_Descriptor descriptor { uri.c_str (), instantiate, cleanup, portEvent, extensionData };
    return index == 0 ? &descriptor : nullptr;
}
