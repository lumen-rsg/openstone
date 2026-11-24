#pragma once

#include "yellow/Value.hpp"

#include <QFont>
#include <QTransform>
#include <QVariantMap>

class QTextLayout;

namespace openstone::desktop {

// Keep line breaks and measurements in grid units across editor panels, windows, and bitmaps.
inline constexpr qreal layoutPixelsPerUnit = 12;

struct TextMeasurement {
    yellow::Area bounds;
    int chars = 0;
    int lines = 0;
};

QFont panelTypeface(const yellow::Font& description);
QVariantMap panelTextProperties(std::string_view text, const yellow::Font& font, const yellow::StringFormat& format);
int layoutPanelText(QTextLayout& layout, const QVariantMap& drawing, QSizeF extent);
QTransform textTransform(bool vertical, qreal layoutWidth, qreal scale);
TextMeasurement measureTextLayout(std::string_view text, yellow::Area area, const yellow::Font& font,
                                  const yellow::StringFormat& format, bool pointOrigin);
yellow::Area measureText(std::string_view text, yellow::Area area, const yellow::Font& font,
                         const yellow::StringFormat& format);

}
