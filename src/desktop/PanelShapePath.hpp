#pragma once

#include "yellow/Value.hpp"
#include <QPainterPath>
#include <QRectF>
#include <cmath>

namespace openstone::desktop {
// Qt Quick uses clockwise angles from +X. QPainterPath uses counterclockwise angles.
inline double panelShapeStartAngle(const yellow::DrawCommand& command)
{
    return command.shape == yellow::ShapeKind::Arc || command.shape == yellow::ShapeKind::Pie ? command.startAngle : 0;
}

inline QPainterPath shapeFillPath(const yellow::DrawCommand& command, const QRectF& area)
{
    using enum yellow::ShapeKind;
    QPainterPath path;
    if (command.shape == Rect) {
        path.addRoundedRect(area, command.cornerRadius, command.cornerRadius);
    } else if (command.shape == Ellipse || std::abs(command.sweepAngle) >= 360) {
        path.addEllipse(area);
    } else if (command.shape == Arc || command.shape == Pie) {
        if (command.shape == Pie) path.moveTo(area.center());
        else path.arcMoveTo(area, -command.startAngle);
        path.arcTo(area, -command.startAngle, -command.sweepAngle);
        path.closeSubpath(); // Arc fills a chord-bounded segment; Pie includes the center.
    }
    return path;
}
}
