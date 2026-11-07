// Compares a plug-in's LV2 panel with its VST3 panel, side by side on one X server: what they draw, pixel for pixel, and
// what a mouse drag on them writes. Then it opens and closes LV2 panels in the ways a host can, including with a second
// one left running.
//
// Usage: <program> <VST3 .so> <LV2 bundle folder>
//
// It needs an X server with a screen of at least kScreenWidth by kScreenHeight, which CMake starts through xvfb-run. It
// doesn't replace looking at a panel in a host: nothing here can say that a panel looks right, only that it's the same.

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Steinberg;
namespace fs = std::filesystem;

DEF_CLASS_IID (FUnknown)
DEF_CLASS_IID (IPluginBase)
DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponentHandler)

namespace {

constexpr int kScreenWidth = 4000;
constexpr int kScreenHeight = 1200;
constexpr int kSecondColumn = 2000;
constexpr int kSecondRow = 400;

int gFailures = 0;
xcb_connection_t* gConnection = nullptr;
xcb_screen_t* gScreen = nullptr;

void check (bool isOk, const std::string& what)
{
    std::printf ("%s %s\n", isOk ? "ok  " : "FAIL", what.c_str ());
    std::fflush (stdout);
    if (!isOk)
        ++gFailures;
}

#define NO_REFCOUNT                                  \
    uint32 PLUGIN_API addRef () override { return 1; } \
    uint32 PLUGIN_API release () override { return 1; }

// One edit as a panel reported it, in the plain value that the port holds.
struct Edit
{
    uint32_t id;
    float plain;

    bool operator== (const Edit& other) const { return id == other.id && std::memcmp (&plain, &other.plain, sizeof (float)) == 0; }
};

class Vst3Host : public Vst::IHostApplication, public IPlugFrame, public Linux::IRunLoop, public Vst::IComponentHandler
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, Vst::IHostApplication::iid))
            *object = static_cast<Vst::IHostApplication*> (this);
        else if (FUnknownPrivate::iidEqual (iid, IPlugFrame::iid))
            *object = static_cast<IPlugFrame*> (this);
        else if (FUnknownPrivate::iidEqual (iid, Linux::IRunLoop::iid))
            *object = static_cast<Linux::IRunLoop*> (this);
        else if (FUnknownPrivate::iidEqual (iid, Vst::IComponentHandler::iid))
            *object = static_cast<Vst::IComponentHandler*> (this);
        else
        {
            *object = nullptr;
            return kNoInterface;
        }
        return kResultOk;
    }

    NO_REFCOUNT

    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        name[0] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (TUID, TUID, void** object) override
    {
        *object = nullptr;
        return kResultFalse;
    }

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

    tresult PLUGIN_API beginEdit (Vst::ParamID) override
    {
        ++begins;
        return kResultOk;
    }

    tresult PLUGIN_API performEdit (Vst::ParamID id, Vst::ParamValue normalized) override
    {
        edits.push_back ({ id, static_cast<float> (controller->normalizedParamToPlain (id, normalized)) });
        return kResultOk;
    }

    tresult PLUGIN_API endEdit (Vst::ParamID) override
    {
        ++ends;
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent (int32) override { return kResultOk; }

    void service ()
    {
        for (const auto& entry : std::vector<std::pair<Linux::IEventHandler*, Linux::FileDescriptor>> (_events))
            entry.first->onFDIsSet (entry.second);
        for (auto* timer : std::vector<Linux::ITimerHandler*> (_timers))
            timer->onTimer ();
    }

    Vst::IEditController* controller { nullptr };
    std::vector<Edit> edits;
    int begins { 0 };
    int ends { 0 };

private:
    std::vector<std::pair<Linux::IEventHandler*, Linux::FileDescriptor>> _events;
    std::vector<Linux::ITimerHandler*> _timers;
};

// What a host records from an LV2 panel.
struct Lv2Host
{
    std::vector<Edit> edits;
    uint32_t firstControl { 0 };
    int grabs { 0 };
    int releases { 0 };
    int resizes { 0 };
    int lastWidth { 0 };
    int lastHeight { 0 };
    int badWrites { 0 };
};

void writePort (LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* buffer)
{
    auto* host = static_cast<Lv2Host*> (controller);
    if (size != sizeof (float) || protocol != 0 || port < host->firstControl)
    {
        ++host->badWrites;
        return;
    }
    host->edits.push_back ({ port - host->firstControl, *static_cast<const float*> (buffer) });
}

int resizeUi (LV2UI_Feature_Handle handle, int width, int height)
{
    auto* host = static_cast<Lv2Host*> (handle);
    ++host->resizes;
    host->lastWidth = width;
    host->lastHeight = height;
    return 0;
}

void touchPort (LV2UI_Feature_Handle handle, uint32_t, bool isGrabbed)
{
    auto* host = static_cast<Lv2Host*> (handle);
    ++(isGrabbed ? host->grabs : host->releases);
}

xcb_window_t makeWindow (int x, int y)
{
    const xcb_window_t window = xcb_generate_id (gConnection);
    xcb_create_window (gConnection, XCB_COPY_FROM_PARENT, window, gScreen->root, static_cast<int16_t> (x), static_cast<int16_t> (y), 1900, 390, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, gScreen->root_visual, 0, nullptr);
    xcb_map_window (gConnection, window);
    std::free (xcb_get_input_focus_reply (gConnection, xcb_get_input_focus (gConnection), nullptr));
    return window;
}

std::vector<xcb_window_t> childrenOf (xcb_window_t window)
{
    std::vector<xcb_window_t> result;
    auto* reply = xcb_query_tree_reply (gConnection, xcb_query_tree (gConnection, window), nullptr);
    if (reply)
    {
        const auto* children = xcb_query_tree_children (reply);
        result.assign (children, children + xcb_query_tree_children_length (reply));
    }
    std::free (reply);
    return result;
}

std::vector<uint8_t> grab (xcb_window_t window, int width, int height)
{
    auto* reply = xcb_get_image_reply (gConnection,
                                       xcb_get_image (gConnection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, static_cast<uint16_t> (width), static_cast<uint16_t> (height), 0xFFFFFFFF),
                                       nullptr);
    std::vector<uint8_t> result;
    if (reply)
        result.assign (xcb_get_image_data (reply), xcb_get_image_data (reply) + xcb_get_image_data_length (reply));
    std::free (reply);
    return result;
}

size_t countDifferentBytes (const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size () != b.size ())
        return std::max (a.size (), b.size ());
    size_t count = 0;
    for (size_t i = 0; i < a.size (); ++i)
        count += a[i] != b[i] ? 1 : 0;
    return count;
}

// A real mouse would need the XTEST extension, and only one panel could have it at a time.
void sendMouse (xcb_window_t window, uint8_t type, int x, int y, uint8_t button, uint16_t state)
{
    xcb_button_press_event_t event {};
    event.response_type = type;
    event.detail = button;
    event.time = XCB_CURRENT_TIME;
    event.root = gScreen->root;
    event.event = window;
    event.event_x = static_cast<int16_t> (x);
    event.event_y = static_cast<int16_t> (y);
    event.state = state;
    event.same_screen = 1;
    xcb_send_event (gConnection, 0, window, 0, reinterpret_cast<const char*> (&event));
    xcb_flush (gConnection);
}

struct ControlPorts
{
    uint32_t first { 0 };
    uint32_t count { 0 };
};

ControlPorts readPorts (const fs::path& turtle)
{
    std::ifstream stream (turtle);
    const std::string text ((std::istreambuf_iterator<char> (stream)), std::istreambuf_iterator<char> ());
    const std::regex pattern ("a lv2:InputPort , lv2:ControlPort ;\\s+lv2:index ([0-9]+)");
    ControlPorts result;
    result.first = 0xFFFFFFFF;
    for (auto it = std::sregex_iterator (text.begin (), text.end (), pattern); it != std::sregex_iterator (); ++it)
    {
        result.first = std::min (result.first, static_cast<uint32_t> (std::stoul ((*it)[1].str ())));
        ++result.count;
    }
    return result;
}

class Lv2Panel
{
public:
    Lv2Panel (const LV2UI_Descriptor* descriptor, const LV2UI_Idle_Interface* idle, xcb_window_t parent, uint32_t firstControl)
    : _descriptor (descriptor), _idle (idle)
    {
        host.firstControl = firstControl;
        _resize.handle = &host;
        _resize.ui_resize = resizeUi;
        _touch.handle = &host;
        _touch.touch = touchPort;

        const LV2_Feature parentFeature { LV2_UI__parent, reinterpret_cast<void*> (static_cast<uintptr_t> (parent)) };
        const LV2_Feature idleFeature { LV2_UI__idleInterface, nullptr };
        const LV2_Feature resizeFeature { LV2_UI__resize, &_resize };
        const LV2_Feature touchFeature { LV2_UI__touch, &_touch };
        const LV2_Feature* const features[] = { &parentFeature, &idleFeature, &resizeFeature, &touchFeature, nullptr };
        handle = descriptor->instantiate (descriptor, "", "", writePort, &host, &widget, features);
    }

    ~Lv2Panel ()
    {
        if (handle)
            _descriptor->cleanup (handle);
    }

    void setPort (uint32_t id, float value)
    {
        _descriptor->port_event (handle, host.firstControl + id, sizeof (float), 0, &value);
    }

    void service () { _idle->idle (handle); }

    Lv2Host host;
    LV2UI_Handle handle { nullptr };
    LV2UI_Widget widget { nullptr };

private:
    const LV2UI_Descriptor* _descriptor;
    const LV2UI_Idle_Interface* _idle;
    LV2UI_Resize _resize {};
    LV2UI_Touch _touch {};
};

const LV2UI_Descriptor* gUi = nullptr;
const LV2UI_Idle_Interface* gIdle = nullptr;
ControlPorts gPorts;

void checkRefusals (xcb_window_t parent)
{
    Lv2Host host;
    LV2UI_Widget widget = nullptr;
    const LV2_Feature parentFeature { LV2_UI__parent, reinterpret_cast<void*> (static_cast<uintptr_t> (parent)) };
    const LV2_Feature idleFeature { LV2_UI__idleInterface, nullptr };

    const LV2_Feature* const noParent[] = { &idleFeature, nullptr };
    check (gUi->instantiate (gUi, "", "", writePort, &host, &widget, noParent) == nullptr, "refuses a host giving no parent window");

    // As REAPER does, which then calls idle anyway, and crashes if a UI refuses.
    const LV2_Feature* const noIdle[] = { &parentFeature, nullptr };
    LV2UI_Handle handle = gUi->instantiate (gUi, "", "", writePort, &host, &widget, noIdle);
    check (handle != nullptr, "opens for a host that doesn't pass ui:idleInterface");
    if (handle)
        gUi->cleanup (handle);

    const std::string display = std::getenv ("DISPLAY");
    setenv ("DISPLAY", ":9999", 1);
    const LV2_Feature* const both[] = { &parentFeature, &idleFeature, nullptr };
    check (gUi->instantiate (gUi, "", "", writePort, &host, &widget, both) == nullptr, "refuses when it can't reach an X server, without crashing");
    setenv ("DISPLAY", display.c_str (), 1);
}

} // namespace

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf ("Usage: <program> <VST3 .so> <LV2 bundle folder>\n");
        return 2;
    }

    int screenNumber = 0;
    gConnection = xcb_connect (nullptr, &screenNumber);
    if (xcb_connection_has_error (gConnection) != 0)
    {
        std::printf ("There's no X server\n");
        return 2;
    }
    auto screens = xcb_setup_roots_iterator (xcb_get_setup (gConnection));
    for (int i = 0; i < screenNumber; ++i)
        xcb_screen_next (&screens);
    gScreen = screens.data;
    if (gScreen->width_in_pixels < kScreenWidth || gScreen->height_in_pixels < kScreenHeight)
    {
        std::printf ("The screen is too small\n");
        return 2;
    }

    // The bundle's name isn't always the plug-in's, because the test puts a copy at a temporary one.
    fs::path binary;
    fs::path turtle;
    for (const auto& entry : fs::directory_iterator (argv[2]))
    {
        if (entry.path ().extension () == ".so")
            binary = fs::absolute (entry.path ());
        else if (entry.path ().extension () == ".ttl" && entry.path ().filename () != "manifest.ttl")
            turtle = entry.path ();
    }

    gPorts = readPorts (turtle);
    check (gPorts.count > 0, "the Turtle has " + std::to_string (gPorts.count) + " controls, from port " + std::to_string (gPorts.first));

    void* lv2Module = dlopen (binary.c_str (), RTLD_LAZY | RTLD_LOCAL);
    void* vst3Module = dlopen (argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!lv2Module || !vst3Module)
    {
        std::printf ("Could not load a binary: %s\n", dlerror ());
        return 2;
    }

    auto getPlugin = reinterpret_cast<const LV2_Descriptor* (*) (uint32_t)> (dlsym (lv2Module, "lv2_descriptor"));
    auto getUi = reinterpret_cast<const LV2UI_Descriptor* (*) (uint32_t)> (dlsym (lv2Module, "lv2ui_descriptor"));
    auto moduleEntry = reinterpret_cast<bool (*) (void*)> (dlsym (vst3Module, "ModuleEntry"));
    auto moduleExit = reinterpret_cast<bool (*) ()> (dlsym (vst3Module, "ModuleExit"));
    auto getFactory = reinterpret_cast<GetFactoryProc> (dlsym (vst3Module, "GetPluginFactory"));
    if (!getPlugin || !getUi || !moduleEntry || !moduleExit || !getFactory || !moduleEntry (vst3Module))
    {
        std::printf ("Missing entry points\n");
        return 2;
    }

    gUi = getUi (0);
    check (gUi != nullptr && getUi (1) == nullptr, "one UI");
    if (!gUi)
        return 1;
    const std::string expectedUri = std::string (getPlugin (0)->URI) + "#ui";
    check (expectedUri == gUi->URI, std::string ("its URI is ") + gUi->URI);
    std::ifstream stream (turtle);
    const std::string turtleText ((std::istreambuf_iterator<char> (stream)), std::istreambuf_iterator<char> ());
    check (turtleText.find ("ui:ui <" + expectedUri + ">") != std::string::npos, "the Turtle names that URI");
    gIdle = static_cast<const LV2UI_Idle_Interface*> (gUi->extension_data (LV2_UI__idleInterface));
    check (gIdle != nullptr, "it has the idle interface");
    if (!gIdle)
        return 1;

    const xcb_window_t vst3Parent = makeWindow (0, 0);
    const xcb_window_t lv2Parent = makeWindow (kSecondColumn, 0);
    const xcb_window_t otherParent = makeWindow (kSecondColumn, kSecondRow);

    checkRefusals (lv2Parent);

    // The VST3's panel, as a host opens it.
    Vst3Host vst3;
    auto* factory = getFactory ();
    TUID controllerId {};
    for (int32 i = 0; i < factory->countClasses (); ++i)
    {
        PClassInfo info;
        factory->getClassInfo (i, &info);
        if (std::strcmp (info.category, kVstComponentControllerClass) == 0)
            std::memcpy (controllerId, info.cid, sizeof (TUID));
    }
    if (factory->createInstance (controllerId, Vst::IEditController::iid, reinterpret_cast<void**> (&vst3.controller)) != kResultOk
        || vst3.controller->initialize (static_cast<Vst::IHostApplication*> (&vst3)) != kResultOk)
    {
        std::printf ("Could not make the VST3's controller\n");
        return 2;
    }
    vst3.controller->setComponentHandler (&vst3);
    IPlugView* view = vst3.controller->createView (Vst::ViewType::kEditor);
    if (!view)
    {
        std::printf ("Could not make the VST3's panel\n");
        return 2;
    }
    view->setFrame (&vst3);
    check (view->attached (reinterpret_cast<void*> (static_cast<uintptr_t> (vst3Parent)), kPlatformTypeX11EmbedWindowID) == kResultOk, "the VST3's panel opens");
    ViewRect size;
    view->getSize (&size);
    const int width = size.getWidth ();
    const int height = size.getHeight ();
    const auto vst3Children = childrenOf (vst3Parent);
    check (vst3Children.size () == 1 && gPorts.count == static_cast<uint32_t> (vst3.controller->getParameterCount ()), "it has one window, and as many parameters as the LV2 has controls");
    if (vst3Children.size () != 1)
        return 1;
    const xcb_window_t vst3Frame = vst3Children[0];

    {
        Lv2Panel panel (gUi, gIdle, lv2Parent, gPorts.first);
        check (panel.handle != nullptr, "the LV2's panel opens");
        if (!panel.handle)
            return 1;

        const auto widget = static_cast<xcb_window_t> (reinterpret_cast<uintptr_t> (panel.widget));
        const auto lv2Children = childrenOf (lv2Parent);
        check (lv2Children.size () == 1 && lv2Children[0] == widget, "the widget it gives the host is a child of the host's window");
        const auto frames = childrenOf (widget);
        check (frames.size () == 1, "with the panel inside it");
        if (frames.size () != 1)
            return 1;
        const xcb_window_t lv2Frame = frames[0];
        check (panel.host.resizes == 1 && panel.host.lastWidth == width && panel.host.lastHeight == height, "it tells a host that offers ui:resize its size");

        auto* geometry = xcb_get_geometry_reply (gConnection, xcb_get_geometry (gConnection, widget), nullptr);
        check (geometry && geometry->width == width && geometry->height == height, "the widget is the panel's size");
        std::free (geometry);

        const auto service = [&] (int milliseconds) {
            const auto end = std::chrono::steady_clock::now () + std::chrono::milliseconds (milliseconds);
            do
            {
                vst3.service ();
                panel.service ();
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            } while (std::chrono::steady_clock::now () < end);
        };

        // Each panel redraws in its own time, so this gives them a while to agree, and they must end up identical.
        const auto samePanels = [&] (const std::string& what) {
            size_t difference = 0;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                service (100);
                difference = countDifferentBytes (grab (vst3Parent, width, height), grab (lv2Parent, width, height));
                if (difference == 0 && attempt >= 2)
                    break;
            }
            check (difference == 0, what + ": " + std::to_string (difference) + " bytes differ");
        };

        samePanels ("at the defaults, the panels are identical");
        const auto defaults = grab (lv2Parent, width, height);

        // Through a port on one side, and the way a host's automation arrives on the other. The same rounding as the
        // audio side, because a port only holds a float.
        const auto setBoth = [&] (uint32_t id, double normalized) {
            const float plain = static_cast<float> (vst3.controller->normalizedParamToPlain (id, normalized));
            panel.setPort (id, plain);
            const double rounded = normalized <= 0.0 ? 0.0 : normalized >= 1.0 ? 1.0 : vst3.controller->plainParamToNormalized (id, plain);
            vst3.controller->setParamNormalized (id, rounded);
        };

        for (const double normalized : { 0.0, 1.0, 0.37 })
        {
            for (uint32_t id = 0; id < gPorts.count; ++id)
                setBoth (id, normalized);
            samePanels ("with every port at " + std::to_string (normalized));
            check (countDifferentBytes (defaults, grab (lv2Parent, width, height)) > 0, "  which isn't what the defaults look like");
        }
        check (panel.host.edits.empty () && panel.host.grabs == 0, "ports moving doesn't make the panel write anything back");

        const std::vector<uint8_t> beforeUnknown = grab (lv2Parent, width, height);
        float ignored = 1.0f;
        gUi->port_event (panel.handle, gPorts.first + gPorts.count, sizeof (float), 0, &ignored);
        gUi->port_event (panel.handle, gPorts.first, sizeof (float), 1234, &ignored);
        gUi->port_event (panel.handle, gPorts.first, 1, 0, &ignored);
        samePanels ("events for other ports, or in other formats, change nothing");
        check (countDifferentBytes (beforeUnknown, grab (lv2Parent, width, height)) == 0, "  and the panel's still as it was");

        // Looks for a knob from the bottom right, away from the menus, by dragging until something is written.
        constexpr int kDrag = 30;
        int knobX = -1;
        int knobY = -1;
        for (int y = height - 8; y > 0 && knobX < 0; y -= 8)
        {
            for (int x = width - 8; x > 0 && knobX < 0; x -= 8)
            {
                sendMouse (lv2Frame, XCB_BUTTON_PRESS, x, y, 1, 0);
                sendMouse (vst3Frame, XCB_BUTTON_PRESS, x, y, 1, 0);
                service (15);
                sendMouse (lv2Frame, XCB_MOTION_NOTIFY, x, y - kDrag, 0, XCB_BUTTON_MASK_1);
                sendMouse (vst3Frame, XCB_MOTION_NOTIFY, x, y - kDrag, 0, XCB_BUTTON_MASK_1);
                service (15);
                sendMouse (lv2Frame, XCB_BUTTON_RELEASE, x, y - kDrag, 1, XCB_BUTTON_MASK_1);
                sendMouse (vst3Frame, XCB_BUTTON_RELEASE, x, y - kDrag, 1, XCB_BUTTON_MASK_1);
                service (15);
                if (!panel.host.edits.empty () || !vst3.edits.empty ())
                {
                    knobX = x;
                    knobY = y;
                }
            }
        }
        check (knobX >= 0, "a drag on the panel writes a value, at " + std::to_string (knobX) + "," + std::to_string (knobY));
        service (50);
        check (!panel.host.edits.empty () && panel.host.edits == vst3.edits, "the LV2 writes the same values, as plain port values, that the VST3 edits: "
                                                                                  + std::to_string (panel.host.edits.size ()) + " and " + std::to_string (vst3.edits.size ()));
        check (panel.host.grabs == vst3.begins && panel.host.releases == vst3.ends && panel.host.grabs == panel.host.releases && panel.host.grabs > 0,
               "it grabs and lets go of the port through ui:touch, like the VST3's begin and end");
        check (panel.host.badWrites == 0, "every write is a float to a control port");
        samePanels ("after the drag, the panels are identical");

        // While a knob is held, a late echo of an old value mustn't move it. Afterwards the port works again.
        if (knobX >= 0)
        {
            const uint32_t id = panel.host.edits.back ().id;
            const float held = panel.host.edits.back ().plain;
            panel.host.edits.clear ();
            vst3.edits.clear ();
            sendMouse (lv2Frame, XCB_BUTTON_PRESS, knobX, knobY, 1, 0);
            sendMouse (vst3Frame, XCB_BUTTON_PRESS, knobX, knobY, 1, 0);
            service (30);
            sendMouse (lv2Frame, XCB_MOTION_NOTIFY, knobX, knobY - 5, 0, XCB_BUTTON_MASK_1);
            sendMouse (vst3Frame, XCB_MOTION_NOTIFY, knobX, knobY - 5, 0, XCB_BUTTON_MASK_1);
            service (30);
            const float stale = static_cast<float> (vst3.controller->normalizedParamToPlain (id, 0.0));
            panel.setPort (id, stale);
            samePanels ("while a knob is held, a late echo from the host doesn't move it");
            sendMouse (lv2Frame, XCB_BUTTON_RELEASE, knobX, knobY - 5, 1, XCB_BUTTON_MASK_1);
            sendMouse (vst3Frame, XCB_BUTTON_RELEASE, knobX, knobY - 5, 1, XCB_BUTTON_MASK_1);
            service (50);
            check (panel.host.edits == vst3.edits, "the rest of that drag writes the same values too");

            // The value this wrote, come back through the host, is no change.
            if (!panel.host.edits.empty ())
                panel.setPort (id, panel.host.edits.back ().plain);
            samePanels ("the echo of a written value leaves it alone");

            setBoth (id, 0.75);
            samePanels ("and after letting go, the port moves the knob again");
            setBoth (id, held <= 0.0f ? 0.5 : 0.25);
        }

        // A second panel on the same run loop, which has to keep running when the first closes.
        panel.host.edits.clear ();
        {
            auto second = std::make_unique<Lv2Panel> (gUi, gIdle, otherParent, gPorts.first);
            check (second->handle != nullptr, "a second instance's panel opens beside it");

            const auto serviceSecond = [&] (Lv2Panel& other, int milliseconds) {
                const auto end = std::chrono::steady_clock::now () + std::chrono::milliseconds (milliseconds);
                do
                {
                    other.service ();
                    std::this_thread::sleep_for (std::chrono::milliseconds (5));
                } while (std::chrono::steady_clock::now () < end);
            };

            serviceSecond (*second, 300);
            second.reset ();
            check (childrenOf (otherParent).empty (), "it closes, and leaves nothing in the host's window");

            // The first still draws. This can only be seen through it responding.
            setBoth (0, 0.9);
            samePanels ("the first panel still responds after a second closes");

            second = std::make_unique<Lv2Panel> (gUi, gIdle, otherParent, gPorts.first);
            serviceSecond (*second, 100);
        }
    }

    // Everything is closed, which ends VSTGUI's connection in this binary and, with no plug-in loaded, the SDK's module.
    check (childrenOf (lv2Parent).empty (), "closing the last panel leaves nothing in the host's window");

    for (int i = 0; i < 20; ++i)
    {
        Lv2Panel panel (gUi, gIdle, lv2Parent, gPorts.first);
        if (!panel.handle)
        {
            check (false, "a panel opens again and again");
            break;
        }
        for (int j = 0; j < 4; ++j)
        {
            panel.service ();
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }
    check (true, "panels opened and closed 20 times");

    {
        Lv2Panel panel (gUi, gIdle, lv2Parent, gPorts.first);
        check (panel.handle != nullptr, "and it still opens after that");
        const auto end = std::chrono::steady_clock::now () + std::chrono::milliseconds (300);
        do
        {
            panel.service ();
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        } while (std::chrono::steady_clock::now () < end);
    }

    view->removed ();
    view->setFrame (nullptr);
    view->release ();
    vst3.controller->setComponentHandler (nullptr);
    vst3.controller->terminate ();
    vst3.controller->release ();
    factory->release ();
    moduleExit ();
    xcb_disconnect (gConnection);

    std::printf ("%d failure(s)\n", gFailures);
    std::fflush (stdout);
    // Not unloading, for the same reason as tools/lv2compare.
    return gFailures == 0 ? 0 : 1;
}
