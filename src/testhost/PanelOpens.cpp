#include "hadronyche/testhost/PanelOpens.h"

#include "hadronyche/testhost/Host.h"

#include "pluginterfaces/gui/iplugview.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace Steinberg;

DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)

namespace Hadronyche {
namespace TestHost {

namespace {

// What a Linux host gives a panel: a plug frame that's also its run loop.
class HostFrame : public IPlugFrame, public Linux::IRunLoop
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
            *object = static_cast<Linux::IRunLoop*> (this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef () override { return 1; }
    uint32 PLUGIN_API release () override { return 1; }

    tresult PLUGIN_API resizeView (IPlugView* view, ViewRect* size) override { return view->onSize (size); }

    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
    {
        _events.emplace_back (handler, fd);
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* handler) override
    {
        _events.erase (std::remove_if (_events.begin (), _events.end (), [handler] (const auto& entry) { return entry.first == handler; }), _events.end ());
        return kResultTrue;
    }

    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* handler, Linux::TimerInterval) override
    {
        _timers.push_back (handler);
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* handler) override
    {
        _timers.erase (std::remove (_timers.begin (), _timers.end (), handler), _timers.end ());
        return kResultTrue;
    }

    // Long enough for the panels' 33 ms timers, whose handlers can change what's registered as they run.
    void pump ()
    {
        for (int i = 0; i < 20; ++i)
        {
            for (const auto& entry : std::vector<std::pair<Linux::IEventHandler*, Linux::FileDescriptor>> (_events))
                entry.first->onFDIsSet (entry.second);
            for (auto* timer : std::vector<Linux::ITimerHandler*> (_timers))
                timer->onTimer ();
            sleepMilliseconds (5);
        }
    }

    size_t countHandlers () const { return _events.size () + _timers.size (); }

private:
    std::vector<std::pair<Linux::IEventHandler*, Linux::FileDescriptor>> _events;
    std::vector<Linux::ITimerHandler*> _timers;
};

int countChildren (xcb_connection_t* connection, xcb_window_t window)
{
    auto* reply = xcb_query_tree_reply (connection, xcb_query_tree (connection, window), nullptr);
    const int count = reply ? xcb_query_tree_children_length (reply) : -1;
    std::free (reply);
    return count;
}

IPlugView* openPanel (PlugBase& plug, HostFrame& frame, xcb_window_t parent)
{
    IPlugView* view = plug.controller->createView (Vst::ViewType::kEditor);
    if (!view)
        return nullptr;

    view->setFrame (&frame);
    if (view->attached (reinterpret_cast<void*> (static_cast<uintptr_t> (parent)), kPlatformTypeX11EmbedWindowID) != kResultOk)
    {
        view->setFrame (nullptr);
        view->release ();
        return nullptr;
    }
    return view;
}

void closePanel (IPlugView* view)
{
    view->removed ();
    view->setFrame (nullptr);
    view->release ();
}

} // namespace

void testPanelOpens ()
{
    int screenNumber = 0;
    xcb_connection_t* connection = xcb_connect (nullptr, &screenNumber);
    const bool connected = xcb_connection_has_error (connection) == 0;
    check ("there's an X server, which xvfb-run can provide", connected);
    if (!connected)
    {
        xcb_disconnect (connection);
        return;
    }

    auto screens = xcb_setup_roots_iterator (xcb_get_setup (connection));
    for (int i = 0; i < screenNumber; ++i)
        xcb_screen_next (&screens);

    const xcb_window_t parent = xcb_generate_id (connection);
    xcb_create_window (connection, XCB_COPY_FROM_PARENT, parent, screens.data->root, 0, 0, 1200, 800, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screens.data->root_visual, 0, nullptr);
    xcb_map_window (connection, parent);
    xcb_flush (connection);

    {
        HostFrame frame;
        PlugBase first;
        PlugBase second;

        IPlugView* view = first.controller->createView (Vst::ViewType::kEditor);
        check ("the editor doesn't offer Wayland, which it can't serve", view && view->isPlatformTypeSupported (kPlatformTypeWaylandSurfaceID) != kResultTrue);
        check ("the editor offers X11", view && view->isPlatformTypeSupported (kPlatformTypeX11EmbedWindowID) == kResultTrue);
        if (view)
            view->release ();

        IPlugView* firstView = openPanel (first, frame, parent);
        check ("a panel opens", firstView != nullptr);
        if (firstView)
        {
            frame.pump ();
            check ("it made a window in the host's", countChildren (connection, parent) == 1);
            check ("it runs on the host's run loop", frame.countHandlers () > 0);

            IPlugView* secondView = openPanel (second, frame, parent);
            check ("a second instance's panel opens beside it", secondView != nullptr);
            frame.pump ();

            closePanel (firstView);
            frame.pump ();
            check ("the first closes, leaving the second running", frame.countHandlers () > 0);

            if (secondView)
                closePanel (secondView);
            frame.pump ();

            // Often, because the X connection starts over each time, and what goes wrong depends on where it lands in
            // memory: VSTGUI leaves Cairo's data about the old one behind unless the panel clears it.
            bool reopened = true;
            for (int i = 0; i < 20 && reopened; ++i)
            {
                firstView = openPanel (first, frame, parent);
                reopened = firstView != nullptr;
                frame.pump ();
                if (firstView)
                    closePanel (firstView);
                frame.pump ();
            }
            check ("a panel opens again and again after them all closing", reopened);
            check ("closing the last leaves nothing on the host's run loop", frame.countHandlers () == 0);
            check ("or in its window", countChildren (connection, parent) == 0);
        }
    }

    xcb_destroy_window (connection, parent);
    xcb_disconnect (connection);
}

} // namespace TestHost
} // namespace Hadronyche
