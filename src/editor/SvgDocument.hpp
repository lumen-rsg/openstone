#pragma once

#include "yellow/Bitmap.hpp"

#include <cstdint>
#include <functional>

namespace openstone {

struct SvgRasterLayout {
    std::int32_t width;
    std::int32_t height;
    double x;
    double y;
    double scaleX;
    double scaleY;
};

struct SvgDocument {
    double width;
    double height;
    std::function<yellow::Bitmap(const SvgRasterLayout&)> rasterize;
};

}
