#pragma once

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Hadronyche {

// Parameters are saved as ID and value pairs, so that newer versions can add parameters and still read older
// projects: int32 version, int32 count, then int32 ID and double normalized value pairs.

inline bool writeParamState (Steinberg::IBStream* stream, Steinberg::int32 version, const double* normalizedValues, Steinberg::int32 count)
{
    Steinberg::IBStreamer streamer (stream, kLittleEndian);
    if (!streamer.writeInt32 (version) || !streamer.writeInt32 (count))
        return false;

    for (Steinberg::int32 id = 0; id < count; ++id)
    {
        if (!streamer.writeInt32 (id) || !streamer.writeDouble (normalizedValues[id]))
            return false;
    }
    return true;
}

// Gets each known parameter, and the version it was saved with, so that a plug-in can convert old values.
using ParamValueReader = std::function<void (Steinberg::Vst::ParamID id, double normalized, Steinberg::int32 version)>;

// A version newer than currentVersion is refused, and so is a stream that ends early, though what came before
// that has been read by then. Unknown IDs and broken values are skipped. On return, the stream is positioned at
// whatever was saved after the parameters.
inline bool readParamState (Steinberg::IBStream* stream, Steinberg::int32 currentVersion, Steinberg::int32 paramCount,
                            const ParamValueReader& onValue, Steinberg::int32* versionOut = nullptr)
{
    Steinberg::IBStreamer streamer (stream, kLittleEndian);

    Steinberg::int32 version = 0;
    Steinberg::int32 count = 0;
    if (!streamer.readInt32 (version) || version < 1 || version > currentVersion || !streamer.readInt32 (count))
        return false;

    for (Steinberg::int32 i = 0; i < count; ++i)
    {
        Steinberg::int32 id = 0;
        double value = 0.0;
        if (!streamer.readInt32 (id) || !streamer.readDouble (value))
            return false;

        if (id >= 0 && id < paramCount && std::isfinite (value))
            onValue (static_cast<Steinberg::Vst::ParamID> (id), std::clamp (value, 0.0, 1.0), version);
    }

    if (versionOut)
        *versionOut = version;
    return true;
}

} // namespace Hadronyche
