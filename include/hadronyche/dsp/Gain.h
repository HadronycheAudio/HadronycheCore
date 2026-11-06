#pragma once

#include <algorithm>
#include <cmath>

namespace Hadronyche {

// At or below this, a Volume control is silent.
constexpr double kVolumeSilenceDb = -60.0;

inline double dbToGain (double db)
{
    return std::pow (10.0, db / 20.0);
}

inline double volumeToGain (double db)
{
    return db <= kVolumeSilenceDb ? 0.0 : dbToGain (db);
}

// A dry/wet law where both play at full level at 50%, so that nothing drops in level in the middle.
inline double dryGain (double percent)
{
    return std::min (1.0, 2.0 * (1.0 - percent / 100.0));
}

inline double wetGain (double percent)
{
    return std::min (1.0, 2.0 * percent / 100.0);
}

} // namespace Hadronyche
