#pragma once

#include <initializer_list>

namespace Hadronyche {

// The sizes of a panel made of titled sections of controls in columns. The first two are what plug-ins differ in,
// and moving any of these moves controls, which the plug-ins' panel tests click at fixed positions.
struct PanelLayout
{
    int sectionHeight { 112 };
    int knobRowOffset { 26 }; // From the top of a section to the title of its knobs.

    int margin { 8 };
    int gap { 6 };
    int columnWidth { 66 };
    int sectionPadding { 6 };
    int knobSize { 44 };
    int titleRowOffset { 4 };
    int labelHeight { 14 };
    int menuHeight { 20 };

    constexpr int sectionWidth (int columns) const { return columns * columnWidth + 2 * sectionPadding; }

    // The width of a row of sections, given how many columns each has.
    constexpr int rowWidth (std::initializer_list<int> sections) const
    {
        int width = 2 * margin - gap;
        for (const int columns : sections)
            width += sectionWidth (columns) + gap;
        return width;
    }

    // The height of a panel with one row of sections.
    constexpr int rowHeight () const { return 2 * margin + sectionHeight; }

    // Where the knobs of a section in the first row go.
    constexpr int knobRow () const { return margin + knobRowOffset; }
};

} // namespace Hadronyche
