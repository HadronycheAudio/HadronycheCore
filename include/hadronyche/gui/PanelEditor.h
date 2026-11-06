#pragma once

#include "hadronyche/gui/PanelLayout.h"
#include "hadronyche/vst/ParamSpec.h"

#include "public.sdk/source/vst/vstguieditor.h"
#include "vstgui/vstgui.h"

#include <vector>

namespace Hadronyche {

struct PanelTheme
{
    VSTGUI::CColor panel;
    VSTGUI::CColor section;
    VSTGUI::CColor frame;
    VSTGUI::CColor accent;
    VSTGUI::CColor text;
    VSTGUI::CColor dimText;
};

// A plug-in window built in code, with no .uidesc file, whose controls follow their parameters. Each control's tag is
// its parameter's ID, and tags from the parameter count upwards are for a plug-in's own controls.
//
// A plug-in adds its controls in buildPanel, and everything else is optional.
class PanelEditor : public Steinberg::Vst::VSTGUIEditor, public VSTGUI::IControlListener
{
public:
    using SpecGetter = const ParamSpec& (*) (Steinberg::Vst::ParamID);

    // The theme has to outlive the editor.
    PanelEditor (Steinberg::Vst::EditController* controller, int width, int height, SpecGetter getSpec, Steinberg::Vst::ParamID paramCount,
                 const PanelTheme& theme, const PanelLayout& layout);

#if SMTG_OS_LINUX
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported (Steinberg::FIDString type) SMTG_OVERRIDE;
#endif
    bool PLUGIN_API open (void* parent, const VSTGUI::PlatformType& platformType) SMTG_OVERRIDE;
    void PLUGIN_API close () SMTG_OVERRIDE;

    void valueChanged (VSTGUI::CControl* control) SMTG_OVERRIDE;
    void controlBeginEdit (VSTGUI::CControl* control) SMTG_OVERRIDE;
    void controlEndEdit (VSTGUI::CControl* control) SMTG_OVERRIDE;

protected:
    struct Binding
    {
        VSTGUI::CControl* control;
        VSTGUI::CTextLabel* valueLabel; // Null for controls that show their value themselves.
    };

    using ViewGroup = std::vector<VSTGUI::CView*>;

    virtual void buildPanel () = 0;

    // After the controls have been updated, every 33 ms.
    virtual void onSync () {}

    // For forgetting pointers to views, which belong to the frame.
    virtual void onClose () {}

    // Adds the next section in the row, and returns the left of its first column.
    int addColumnSection (int columns, const char* title);

    void addSection (const VSTGUI::CRect& area, const char* title);

    // The title, control and value, which can be hidden together when two share a column.
    ViewGroup addKnob (int x, int y, Steinberg::Vst::ParamID id, const char* title, bool bipolar = false);

    // A menu lined up with the knobs, with a title above it.
    ViewGroup addMenu (int x, int y, Steinberg::Vst::ParamID id, const char* title);

    VSTGUI::CControl* addMenu (const VSTGUI::CRect& area, Steinberg::Vst::ParamID id);
    void addToggle (const VSTGUI::CRect& area, Steinberg::Vst::ParamID id, const char* title);

    VSTGUI::CTextLabel* createLabel (const VSTGUI::CRect& area, const char* text, const VSTGUI::CColor& color, VSTGUI::CHoriTxtAlign align = VSTGUI::kCenterText) const;

    const std::vector<Binding>& getBindings () const { return _bindings; }

private:
    void syncControls ();

    SpecGetter _getSpec;
    Steinberg::Vst::ParamID _paramCount;
    const PanelTheme& _theme;
    PanelLayout _layout;
    int _nextSectionX { 0 };
    std::vector<Binding> _bindings;
    VSTGUI::SharedPointer<VSTGUI::CVSTGUITimer> _syncTimer;
};

} // namespace Hadronyche
