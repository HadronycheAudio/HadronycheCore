#pragma once

namespace Hadronyche {
namespace TestHost {

// On Linux, opens and closes the real editor inside a host-style window on an X server. It doesn't look at or click
// anything, but a panel that opens this far has connected to the server and is running on the host's run loop, which
// nothing else in a test program reaches. Run it through xvfb-run where there's no display.
void testPanelOpens ();

} // namespace TestHost
} // namespace Hadronyche
