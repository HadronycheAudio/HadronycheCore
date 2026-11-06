#include "hadronyche/gui/PanelEditor.h"

#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#if SMTG_OS_LINUX
#include "public.sdk/source/vst/vstgui_linux_runloop_support.h"
#include "vstgui/lib/platform/linux/cairographicscontext.h"
#include "vstgui/lib/platform/linux/linuxfactory.h"
#include "vstgui/lib/platform/linux/x11platform.h"
#include "vstgui/lib/platform/platform_x11.h"

#include <cairo/cairo-xcb.h>
#include <xcb/xcb.h>
#endif

#include <cmath>
#include <cstring>

using namespace VSTGUI;

namespace Hadronyche {

namespace {

#if SMTG_OS_LINUX
// VSTGUI only connects to the X server for a frame given a run loop, and that run loop already has to be installed.
// Without a configuration, the frame still opens and crashes on its first call to the server. A host provides the run
// loop through its plug frame, or through its context, which the SDK has already handled by the time a panel opens.
//
// VSTGUI has one run loop and one connection per process, so a run loop that came from a plug frame serves every panel
// until the last one closes, and is let go of then. It would otherwise outlive that frame, and later panels would run
// on a loop no longer serviced.
int gOpenPanels = 0;
bool gRunLoopIsFromPlugFrame = false;

// Cairo remembers each connection, and VSTGUI's X11 code disconnects without telling it. If a later connection lands on
// the same address, Cairo mistakes it for the old one and aborts, the next time a panel opens. There's no way to do this
// between the last frame closing and VSTGUI disconnecting, so endConnection holds the connection open across that.
void finishCairoDevice (xcb_connection_t* connection)
{
    const xcb_screen_t* screen = xcb_setup_roots_iterator (xcb_get_setup (connection)).data;

    xcb_visualtype_t* visual = nullptr;
    for (auto depths = xcb_screen_allowed_depths_iterator (screen); depths.rem > 0 && !visual; xcb_depth_next (&depths))
    {
        for (auto visuals = xcb_depth_visuals_iterator (depths.data); visuals.rem > 0 && !visual; xcb_visualtype_next (&visuals))
        {
            if (visuals.data->visual_id == screen->root_visual)
                visual = visuals.data;
        }
    }
    if (!visual)
        return;

    // There's one device per connection, so any surface leads to it.
    cairo_surface_t* surface = cairo_xcb_surface_create (connection, screen->root, visual, 1, 1);
    cairo_device_t* device = cairo_device_reference (cairo_surface_get_device (surface));
    cairo_surface_destroy (surface);

    getPlatformFactory ().asLinuxFactory ()->getCairoGraphicsDeviceFactory ().removeDevice (device);
    cairo_device_finish (device);
    cairo_device_destroy (device);
}

void endConnection ()
{
    finishCairoDevice (X11::RunLoop::instance ().getXcbConnection ());
    X11::RunLoop::exit ();

    if (gRunLoopIsFromPlugFrame)
    {
        getPlatformFactory ().asLinuxFactory ()->setRunLoop (nullptr);
        gRunLoopIsFromPlugFrame = false;
    }
}

bool openFrame (CFrame* frame, void* parent, const PlatformType& platformType, Steinberg::IPlugFrame* plugFrame)
{
    const auto* factory = getPlatformFactory ().asLinuxFactory ();
    if (!factory->getRunLoop ())
    {
        if (!Steinberg::Linux::setupVSTGUIRunloop (plugFrame))
            return false;
        gRunLoopIsFromPlugFrame = true;
    }

    if (gOpenPanels == 0)
        X11::RunLoop::init ();

    X11::FrameConfig config;
    config.runLoop = factory->getRunLoop ();
    if (frame->open (parent, platformType, &config))
    {
        ++gOpenPanels;
        return true;
    }

    if (gOpenPanels == 0)
        endConnection ();
    return false;
}

void frameClosed ()
{
    if (--gOpenPanels == 0)
        endConnection ();
}
#else
bool openFrame (CFrame* frame, void* parent, const PlatformType& platformType, Steinberg::IPlugFrame*)
{
    return frame->open (parent, platformType);
}

void frameClosed () {}
#endif

// A titled background box. Controls are added on top of it.
class SectionView : public CView
{
public:
    SectionView (const CRect& size, const char* title, const PanelTheme& theme, int titleRowOffset)
    : CView (size), _title (title), _theme (theme), _titleRowOffset (titleRowOffset)
    {
        setMouseEnabled (false);
    }

    void draw (CDrawContext* context) override
    {
        const auto bounds = getViewSize ();
        context->setFillColor (_theme.section);
        context->setFrameColor (_theme.frame);
        context->setLineWidth (1.0);
        context->drawRect (bounds, kDrawFilledAndStroked);

        auto titleArea = bounds;
        titleArea.top += _titleRowOffset;
        titleArea.setHeight (18);
        context->setFont (kNormalFont);
        context->setFontColor (_theme.text);
        context->drawString (_title.data (), titleArea);

        setDirty (false);
    }

private:
    UTF8String _title;
    const PanelTheme& _theme;
    int _titleRowOffset;
};

} // namespace

PanelEditor::PanelEditor (Steinberg::Vst::EditController* controller, int width, int height, SpecGetter getSpec, Steinberg::Vst::ParamID paramCount,
                          const PanelTheme& theme, const PanelLayout& layout)
: VSTGUIEditor (controller), _getSpec (getSpec), _paramCount (paramCount), _theme (theme), _layout (layout)
{
    Steinberg::ViewRect size (0, 0, width, height);
    setRect (size);
}

CTextLabel* PanelEditor::createLabel (const CRect& area, const char* text, const CColor& color, CHoriTxtAlign align) const
{
    auto* label = new CTextLabel (area, text);
    label->setFont (kNormalFontSmall);
    label->setFontColor (color);
    label->setHoriAlign (align);
    label->setStyle (CParamDisplay::kNoFrame);
    label->setTransparency (true);
    label->setMouseEnabled (false);
    return label;
}

void PanelEditor::addSection (const CRect& area, const char* title)
{
    frame->addView (new SectionView (area, title, _theme, _layout.titleRowOffset));
}

int PanelEditor::addColumnSection (int columns, const char* title)
{
    const int left = _nextSectionX;
    addSection (CRect (left, _layout.margin, left + _layout.sectionWidth (columns), _layout.margin + _layout.sectionHeight), title);
    _nextSectionX += _layout.sectionWidth (columns) + _layout.gap;
    return left + _layout.sectionPadding;
}

PanelEditor::ViewGroup PanelEditor::addKnob (int x, int y, Steinberg::Vst::ParamID id, const char* title, bool bipolar)
{
    auto* titleLabel = createLabel (CRect (x, y, x + _layout.columnWidth, y + _layout.labelHeight), title, _theme.dimText);
    frame->addView (titleLabel);

    const int knobX = x + (_layout.columnWidth - _layout.knobSize) / 2;
    const int knobY = y + _layout.labelHeight + 2;
    const CRect knobArea (knobX, knobY, knobX + _layout.knobSize, knobY + _layout.knobSize);

    int32_t style = CKnob::kCoronaDrawing | CKnob::kCoronaOutline;
    if (bipolar)
        style |= CKnob::kCoronaFromCenter;

    auto* knob = new CKnob (knobArea, this, static_cast<int32_t> (id), nullptr, nullptr, CPoint (0, 0), style);
    knob->setCoronaColor (_theme.accent);
    knob->setColorHandle (_theme.text);
    knob->setColorShadowHandle (_theme.frame);
    knob->setHandleLineWidth (2.0);
    knob->setCoronaInset (2.0);

    const auto& spec = _getSpec (id);
    knob->setDefaultValue (static_cast<float> (plainToNormalized (spec, spec.defaultValue)));
    frame->addView (knob);

    const int valueY = knobY + _layout.knobSize + 2;
    auto* value = createLabel (CRect (x, valueY, x + _layout.columnWidth, valueY + _layout.labelHeight), "", _theme.text);
    frame->addView (value);

    _bindings.push_back ({ knob, value });
    return { titleLabel, knob, value };
}

PanelEditor::ViewGroup PanelEditor::addMenu (int x, int y, Steinberg::Vst::ParamID id, const char* title)
{
    auto* titleLabel = createLabel (CRect (x, y, x + _layout.columnWidth, y + _layout.labelHeight), title, _theme.dimText);
    frame->addView (titleLabel);

    const int menuY = y + _layout.labelHeight + 2 + (_layout.knobSize - _layout.menuHeight) / 2;
    auto* menu = addMenu (CRect (x + 1, menuY, x + _layout.columnWidth - 1, menuY + _layout.menuHeight), id);
    return { titleLabel, menu };
}

CControl* PanelEditor::addMenu (const CRect& area, Steinberg::Vst::ParamID id)
{
    auto* menu = new COptionMenu (area, this, static_cast<int32_t> (id));
    const auto& spec = _getSpec (id);
    for (int i = 0; i < spec.labelCount; ++i)
        menu->addEntry (spec.labels[i]);
    menu->setCurrent (0); // A new menu has no selection, and would stay blank at a value of 0.

    menu->setFont (kNormalFontSmall);
    menu->setFontColor (_theme.text);
    menu->setBackColor (_theme.panel);
    menu->setFrameColor (_theme.frame);
    menu->setHoriAlign (kCenterText);
    frame->addView (menu);

    _bindings.push_back ({ menu, nullptr });
    return menu;
}

void PanelEditor::addToggle (const CRect& area, Steinberg::Vst::ParamID id, const char* title)
{
    auto* toggle = new CCheckBox (area, this, static_cast<int32_t> (id), title);
    toggle->setFont (kNormalFontSmall);
    toggle->setFontColor (_theme.text);
    toggle->setBoxFillColor (_theme.panel);
    toggle->setBoxFrameColor (_theme.frame);
    toggle->setCheckMarkColor (_theme.accent);
    frame->addView (toggle);

    _bindings.push_back ({ toggle, nullptr });
}

#if SMTG_OS_LINUX
// A Wayland surface needs more from the host than a panel gets, so a host has to use X11.
Steinberg::tresult PLUGIN_API PanelEditor::isPlatformTypeSupported (Steinberg::FIDString type)
{
    if (std::strcmp (type, Steinberg::kPlatformTypeWaylandSurfaceID) == 0)
        return Steinberg::kInvalidArgument;
    return VSTGUIEditor::isPlatformTypeSupported (type);
}
#endif

bool PLUGIN_API PanelEditor::open (void* parent, const PlatformType& platformType)
{
    Steinberg::ViewRect size = getRect ();
    const CRect bounds (0, 0, size.getWidth (), size.getHeight ());

    frame = new CFrame (bounds, this);
    frame->setBackgroundColor (_theme.panel);

    _nextSectionX = _layout.margin;
    buildPanel ();

    if (!openFrame (frame, parent, platformType, plugFrame))
    {
        _bindings.clear ();
        frame->forget ();
        frame = nullptr;
        return false;
    }

    syncControls ();

    // Keeps the panel up to date with automation and state changes.
    _syncTimer = makeOwned<CVSTGUITimer> ([this] (CVSTGUITimer*) { syncControls (); }, 33);
    return true;
}

void PLUGIN_API PanelEditor::close ()
{
    _syncTimer = nullptr;
    _bindings.clear ();
    onClose ();

    if (frame)
    {
        frame->close (); // This also releases the frame.
        frame = nullptr;
        frameClosed ();
    }
}

void PanelEditor::syncControls ()
{
    auto* controller = getController ();

    for (const auto& binding : _bindings)
    {
        const auto id = static_cast<Steinberg::Vst::ParamID> (binding.control->getTag ());
        const auto value = controller->getParamNormalized (id);

        if (std::abs (binding.control->getValueNormalized () - static_cast<float> (value)) > 1e-5f)
        {
            binding.control->setValueNormalized (static_cast<float> (value));
            binding.control->invalid ();
        }

        if (!binding.valueLabel)
            continue;

        Steinberg::Vst::String128 text {};
        controller->getParamStringByValue (id, value, text);

        char ascii[128] {};
        Steinberg::UString (text, str16BufferSize (Steinberg::Vst::String128)).toAscii (ascii, sizeof (ascii));

        UTF8String display (ascii);
        const auto& spec = _getSpec (id);
        if (spec.units[0] != '\0')
        {
            display += " ";
            display += spec.units;
        }
        binding.valueLabel->setText (display);
    }

    onSync ();
}

void PanelEditor::valueChanged (CControl* control)
{
    const auto id = static_cast<Steinberg::Vst::ParamID> (control->getTag ());
    const double value = control->getValueNormalized ();

    getController ()->setParamNormalized (id, value);
    getController ()->performEdit (id, value);
}

void PanelEditor::controlBeginEdit (CControl* control)
{
    if (static_cast<Steinberg::Vst::ParamID> (control->getTag ()) < _paramCount)
        beginEdit (control->getTag ());
}

void PanelEditor::controlEndEdit (CControl* control)
{
    if (static_cast<Steinberg::Vst::ParamID> (control->getTag ()) < _paramCount)
        endEdit (control->getTag ());
}

} // namespace Hadronyche
