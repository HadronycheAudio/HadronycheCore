#include "Common.h"

#include <mutex>

// From the SDK's linuxmain.cpp.
extern "C" bool ModuleEntry (void*);
extern "C" bool ModuleExit ();

namespace Hadronyche {
namespace Lv2 {

// The handle is null on purpose. On Linux VSTGUI only uses it to find a resources folder, by removing three levels from
// the binary's path as if it were in a .vst3 bundle, and it crashes when a shallower LV2 path runs out, such as
// /opt/Name.lv2/Name.so. The panels load nothing from there, so without a handle it doesn't look.
static std::mutex gModuleMutex; // The SDK's count of module users isn't safe across threads.

bool enterModule ()
{
    const std::lock_guard<std::mutex> lock (gModuleMutex);
    if (ModuleEntry (nullptr))
        return true;

    ModuleExit ();
    return false;
}

void exitModule ()
{
    const std::lock_guard<std::mutex> lock (gModuleMutex);
    ModuleExit ();
}

} // namespace Lv2
} // namespace Hadronyche
