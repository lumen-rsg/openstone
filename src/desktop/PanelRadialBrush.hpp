#pragma once

#include "yellow/Value.hpp"
#include <QImage>
#include <QPainterPath>
#include <QRectF>

namespace openstone::desktop {
QPainterPath radialBoundaryPath(const yellow::RadialPath &boundary);
QImage rasterizeRadialBrush(const yellow::RadialGradientBrush &brush, const QRectF &coverage, QSize size);
} // namespace openstone::desktop
