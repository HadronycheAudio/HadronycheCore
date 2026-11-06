#include "hadronyche/gui/PanelEditor.h"

#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <cmath>

using namespace VSTGUI;

namespace Hadronyche {

namespace {

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

bool PLUGIN_API PanelEditor::open (void* parent, const PlatformType& platformType)
{
    Steinberg::ViewRect size = getRect ();
    const CRect bounds (0, 0, size.getWidth (), size.getHeight ());

    frame = new CFrame (bounds, this);
    frame->setBackgroundColor (_theme.panel);

    _nextSectionX = _layout.margin;
    buildPanel ();

    if (!frame->open (parent, platformType))
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
