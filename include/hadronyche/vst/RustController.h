#pragma once

#include "hadronyche/vst/RustPlugin.h"
#include "hadronyche/vst/SpecController.h"

namespace Hadronyche {

// An edit controller whose parameters and saved state are the Rust side's. A plug-in's controller adds its class ID
// and its editor, as with SpecController.
class RustController : public SpecController
{
protected:
    RustController () : SpecController (RustPlugin::getSpec, RustPlugin::get ().paramCount (), RustPlugin::readState) {}
};

} // namespace Hadronyche
