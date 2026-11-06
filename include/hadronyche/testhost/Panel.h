#pragma once

// For driving a plug-in's real editor in an off-screen window. Separate from the rest because of windows.h.

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

} // namespace TestHost
} // namespace Hadronyche
