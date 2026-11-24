#pragma once

#include "yellow/Value.hpp"

namespace openstone::yellow {

[[nodiscard]] bool hasArea(Area area);
[[nodiscard]] Area intersectAreas(Area first, Area second);
[[nodiscard]] ClipRegion regionOf(Area area);
[[nodiscard]] ClipRegion mapRegion(const ViewMapping& mapping, Area area);
[[nodiscard]] bool regionEmpty(const ClipRegion& region);
[[nodiscard]] bool regionContains(const ClipRegion& region, green::Point point);
[[nodiscard]] ClipRegion combineClip(const ClipRegion& current, const ClipRegion& operand, std::int32_t mode);

}
