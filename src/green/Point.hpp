#pragma once

namespace openstone::green {

struct Point {
    float x = 0.0F;
    float y = 0.0F;

    bool operator==(const Point&) const = default;
};

}
