#include "hadronyche/gui/PanelEditor.h"

// That the header stands by itself, and that a plug-in only has to provide buildPanel.
class MinimalEditor : public Hadronyche::PanelEditor
{
public:
    using PanelEditor::PanelEditor;

private:
    void buildPanel () override {}
};
