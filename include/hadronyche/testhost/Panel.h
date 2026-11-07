#pragma once

// For driving a plug-in's real editor in an off-screen window. Separate from the rest because of windows.h.

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Hadronyche {
namespace TestHost {

inline void pump ()
{
    MSG message;
    while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
}

// Saves the window as a BMP file, in the folder that the environment variable names, if it's set.
inline void saveScreenshot (HWND window, int width, int height, const char* folderVariable, const char* name)
{
    char folder[1024] {};
    if (GetEnvironmentVariableA (folderVariable, folder, sizeof (folder)) == 0)
        return;

    HDC screen = GetDC (nullptr);
    HDC context = CreateCompatibleDC (screen);
    HBITMAP bitmap = CreateCompatibleBitmap (screen, width, height);
    SelectObject (context, bitmap);
    PrintWindow (window, context, PW_RENDERFULLCONTENT);

    BITMAPINFOHEADER info {};
    info.biSize = sizeof (info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::vector<char> pixels (static_cast<size_t> (width) * static_cast<size_t> (height) * 4);
    GetDIBits (context, bitmap, 0, static_cast<UINT> (height), pixels.data (), reinterpret_cast<BITMAPINFO*> (&info), DIB_RGB_COLORS);

    BITMAPFILEHEADER header {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof (header) + sizeof (info);
    header.bfSize = header.bfOffBits + static_cast<DWORD> (pixels.size ());

    std::ofstream file (std::filesystem::path (folder) / (std::string (name) + ".bmp"), std::ios::binary);
    file.write (reinterpret_cast<const char*> (&header), sizeof (header));
    file.write (reinterpret_cast<const char*> (&info), sizeof (info));
    file.write (pixels.data (), static_cast<std::streamsize> (pixels.size ()));

    DeleteObject (bitmap);
    DeleteDC (context);
    ReleaseDC (nullptr, screen);
}

// A plug-in's real editor in an off-screen window, which a panel test drives with mouse messages on its real controls.
// It doesn't touch the real mouse. The checks stay in the test: view is null when the controller made no editor, and
// editorWindow when opening it made no window.
class PanelWindow
{
public:
    PanelWindow (Steinberg::Vst::IEditController* controller, const wchar_t* className)
    : view (controller->createView (Steinberg::Vst::ViewType::kEditor)), _className (className)
    {
    }

    ~PanelWindow ()
    {
        if (_opened && !_closed)
            close ();
        if (view)
            view->release ();
        pump ();
        if (window)
            DestroyWindow (window);
        if (_classRegistered)
            UnregisterClassW (_className, GetModuleHandleW (nullptr));
    }

    PanelWindow (const PanelWindow&) = delete;
    PanelWindow& operator= (const PanelWindow&) = delete;

    // Attaches the editor to a new window off the screen, and says whether the editor agreed.
    bool open ()
    {
        view->getSize (&size);

        WNDCLASSW windowClass {};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW (nullptr);
        windowClass.lpszClassName = _className;
        _classRegistered = RegisterClassW (&windowClass) != 0;

        window = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, _className, L"", WS_POPUP, -4000, -4000,
                                  size.getWidth (), size.getHeight (), nullptr, nullptr, windowClass.hInstance, nullptr);
        ShowWindow (window, SW_SHOWNOACTIVATE);

        _opened = true;
        const bool attached = view->attached (window, Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk;
        editorWindow = FindWindowExW (window, nullptr, nullptr, nullptr);
        return attached;
    }

    // Detaches the editor, and says whether it agreed.
    bool close ()
    {
        _closed = true;
        return view->removed () == Steinberg::kResultOk;
    }

    // Calls step and pumps messages for a time, in real time rather than as fast as possible, because the panel updates
    // from a timer.
    template <typename Step>
    void runFor (double seconds, Step step)
    {
        const ULONGLONG end = GetTickCount64 () + static_cast<ULONGLONG> (seconds * 1000.0);
        while (GetTickCount64 () < end)
        {
            step ();
            pump ();
            Sleep (1);
        }
    }

    void click (int x, int y)
    {
        SendMessageW (editorWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (x, y));
        pump ();
        SendMessageW (editorWindow, WM_LBUTTONUP, 0, MAKELPARAM (x, y));
        pump ();
    }

    // Presses at a point and drags 100 pixels to the left, in 20 steps, as a user turning a knob down. With no host to
    // choose otherwise, knobs follow the mouse's angle around their centre, so a drag from a little above a knob's
    // centre ends about 45 degrees in, which is 1/6 of its range, and a little more.
    void dragLeft (int x, int y)
    {
        SendMessageW (editorWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (x, y));
        pump ();
        for (int i = 1; i <= 20; ++i)
        {
            SendMessageW (editorWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM (x - i * 5, y));
            pump ();
        }
        SendMessageW (editorWindow, WM_LBUTTONUP, 0, MAKELPARAM (x - 100, y));
        pump ();
    }

    // Saves the panel as a BMP file, in the folder that the environment variable names, if it's set.
    void saveScreenshot (const char* folderVariable, const char* name)
    {
        TestHost::saveScreenshot (window, size.getWidth (), size.getHeight (), folderVariable, name);
    }

    Steinberg::IPlugView* view { nullptr };
    Steinberg::ViewRect size;
    HWND window { nullptr };
    HWND editorWindow { nullptr };

private:
    const wchar_t* _className;
    bool _classRegistered { false };
    bool _opened { false };
    bool _closed { false };
};

} // namespace TestHost
} // namespace Hadronyche
