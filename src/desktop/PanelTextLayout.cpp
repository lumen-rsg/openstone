#include "PanelTextLayout.hpp"

#include <QFontDatabase>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace {

qreal alignedOffset(qreal available, qreal extent, Qt::Alignment alignment, Qt::AlignmentFlag center,
                    Qt::AlignmentFlag end)
{
    if (alignment.testFlag(center)) {
        return (available - extent) / 2;
    }
    return alignment.testFlag(end) ? available - extent : 0;
}

}

namespace openstone::desktop {

int layoutPanelText(QTextLayout& layout, const QVariantMap& drawing, QSizeF extent)
{
    auto text = drawing.value("text").toString();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', QChar::LineSeparator).replace(u'\n', QChar::LineSeparator);
    auto font = drawing.value("font").value<QFont>();
    const auto pixels = std::clamp(drawing.value("size").toReal() * layoutPixelsPerUnit, 1.0, 16384.0);
    font.setPixelSize(static_cast<int>(std::lround(pixels)));

    const bool vertical = drawing.value("vertical").toBool();
    const bool rtl = drawing.value("rtl").toBool();
    const auto alignment = Qt::Alignment::fromInt(drawing.value("alignment").toInt());
    QTextOption options;
    // Use explicit positions below so that physical alignment is independent of bidi ordering.
    options.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
    options.setTextDirection(rtl && !vertical ? Qt::RightToLeft : Qt::LeftToRight);
    options.setWrapMode(drawing.value("wrap").toBool() ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                                       : QTextOption::NoWrap);
    QTextOption::Flags flags;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    flags.setFlag(QTextOption::ShowDefaultIgnorables, drawing.value("showControls").toBool());
#endif
    flags.setFlag(QTextOption::IncludeTrailingSpaces, drawing.value("includeTrailingSpaces").toBool());
    options.setFlags(flags);

    layout.setText(text);
    layout.setFont(font);
    layout.setTextOption(options);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    const qreal lineWidth = vertical ? extent.height() : extent.width();
    const qreal blockExtent = vertical ? extent.width() : extent.height();
    const bool wholeLinesOnly = drawing.value("wholeLinesOnly").toBool();
    qreal blockSize = 0;
    int lineCount = 0;
    for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(lineWidth);
        if (wholeLinesOnly && blockSize + line.height() > blockExtent) break;
        blockSize += line.height();
        ++lineCount;
    }
    layout.endLayout();

    const qreal blockStart = vertical ? alignedOffset(extent.width(), blockSize, alignment, Qt::AlignHCenter, Qt::AlignRight)
                                      : alignedOffset(extent.height(), blockSize, alignment, Qt::AlignVCenter, Qt::AlignBottom);
    qreal offset = 0;
    for (int i = 0; i < lineCount; ++i) {
        auto line = layout.lineAt(i);
        if (vertical) {
            const auto column = rtl ? blockSize - offset - line.height() : offset;
            const auto start = alignedOffset(extent.height(), line.naturalTextWidth(), alignment, Qt::AlignVCenter, Qt::AlignBottom);
            line.setPosition({start, extent.width() - blockStart - column - line.height()});
        } else {
            const auto start = alignedOffset(extent.width(), line.naturalTextWidth(), alignment, Qt::AlignHCenter, Qt::AlignRight);
            line.setPosition({start, blockStart + offset});
        }
        offset += line.height();
    }
    return lineCount;
}

QTransform textTransform(bool vertical, qreal layoutWidth, qreal scale)
{
    auto transform = QTransform::fromScale(scale, scale);
    if (vertical) {
        transform.translate(layoutWidth, 0).rotate(90);
    }
    return transform;
}

QFont panelTypeface(const yellow::Font& description)
{
    QFont font;
    QStringList families;
    const auto addFamily = [&](const std::string& face) {
        const auto family = QString::fromUtf8(face.data(), face.size());
        if (!family.isEmpty() && QFontDatabase::hasFamily(family) && !families.contains(family, Qt::CaseInsensitive)) {
            families.push_back(family);
        }
    };
    addFamily(description.face);
    addFamily(description.alternateFace);
    for (const auto& face : description.fallbackFaces) addFamily(face);
    if (!families.isEmpty()) {
        font.setFamilies(families);
    }
    return font;
}

QVariantMap panelTextProperties(std::string_view text, const yellow::Font& description,
                                const yellow::StringFormat& format)
{
    auto font = panelTypeface(description);
    font.setBold(description.style.bold);
    font.setItalic(description.style.italic);
    font.setUnderline(description.style.underline);
    font.setStrikeOut(description.style.strikeout);
    if (format.noFontFallback) font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));
    using yellow::HorizontalAlignment;
    using yellow::VerticalAlignment;
    const auto horizontal = format.horizontal == HorizontalAlignment::Center ? Qt::AlignHCenter
                          : format.horizontal == HorizontalAlignment::Right ? Qt::AlignRight : Qt::AlignLeft;
    const auto vertical = format.vertical == VerticalAlignment::Center ? Qt::AlignVCenter
                        : format.vertical == VerticalAlignment::Bottom ? Qt::AlignBottom : Qt::AlignTop;
    return {{"text", QString::fromUtf8(text.data(), text.size())}, {"font", font}, {"size", description.size},
            {"alignment", (horizontal | vertical).toInt()}, {"wrap", format.wrap},
            {"rtl", format.rightToLeft}, {"vertical", format.verticalText}, {"clipText", format.clip},
            {"showControls", format.showControls}, {"includeTrailingSpaces", format.includeTrailingSpaces},
            {"wholeLinesOnly", format.wholeLinesOnly}};
}

namespace {

int codePointCount(QStringView text)
{
    return static_cast<int>(std::ranges::count_if(text, [](QChar unit) {
        return !unit.isLowSurrogate();
    }));
}

int fittedCharacters(const QTextLayout& layout, QStringView text, const QTextLine& line,
                     qreal left, qreal right)
{
    const int first = line.textStart();
    const int end = first + line.textLength();
    if (left <= line.x() && right >= line.x() + line.naturalTextWidth()) {
        return codePointCount(text.mid(first, line.textLength()));
    }

    // Qt quantizes line positions to 1/64 of a layout pixel. Allow that rounding at the clip edge.
    constexpr qreal edgeTolerance = 1.0 / 64.0;
    // Treat a shaped cluster as a unit so clipping cannot count half a character.
    int count = 0;
    for (int cursor = first; cursor < end;) {
        const auto next = std::min(end, layout.nextCursorPosition(cursor));
        const auto a = line.cursorToX(cursor);
        const auto b = line.cursorToX(cursor, QTextLine::Trailing);
        if (std::min(a, b) >= left - edgeTolerance && std::max(a, b) <= right + edgeTolerance) {
            count += codePointCount(text.mid(cursor, next - cursor));
        }
        cursor = next;
    }
    return count;
}

}

TextMeasurement measureTextLayout(std::string_view text, yellow::Area area, const yellow::Font& font,
                                  const yellow::StringFormat& format, bool pointOrigin)
{
    TextMeasurement result;
    const auto bounds = QRectF(area.x, area.y, area.width, area.height).normalized();
    if (text.empty() || (!pointOrigin && bounds.isEmpty())) return result;

    // Point layout uses zero extent for alignment, as in the renderer.
    const auto extent = pointOrigin ? QSizeF(0, 0) : bounds.size() * layoutPixelsPerUnit;
    QTextLayout layout;
    const auto lineCount = layoutPanelText(layout, panelTextProperties(text, font, format), extent);
    const auto content = layout.text();
    const auto inlineLimit = format.verticalText ? extent.height() : extent.width();
    const auto blockLimit = format.verticalText ? extent.width() : extent.height();
    const bool clip = format.clip && !pointOrigin;
    QRectF measured;
    for (int i = 0; i < lineCount; ++i) {
        const auto line = layout.lineAt(i);
        const auto lineRight = line.x() + line.naturalTextWidth();
        const auto lineBottom = line.y() + line.height();
        const auto left = clip ? std::max(line.x(), 0.0) : line.x();
        const auto right = clip ? std::min(lineRight, inlineLimit) : lineRight;
        const auto top = clip ? std::max(line.y(), 0.0) : line.y();
        const auto bottom = clip ? std::min(lineBottom, blockLimit) : lineBottom;
        if (bottom <= top || right < left || (right == left && lineRight > line.x())) continue;

        ++result.lines;
        result.chars += fittedCharacters(layout, content, line, left, right);
        if (right > left) measured = measured.united({left, top, right - left, bottom - top});
    }
    if (!measured.isEmpty()) {
        const auto transform = textTransform(format.verticalText, extent.width(), 1 / layoutPixelsPerUnit);
        const auto box = transform.mapRect(measured).translated(bounds.topLeft());
        result.bounds = {static_cast<float>(box.x()), static_cast<float>(box.y()),
                         static_cast<float>(box.width()), static_cast<float>(box.height())};
    }
    return result;
}

yellow::Area measureText(std::string_view text, yellow::Area area, const yellow::Font& font,
                         const yellow::StringFormat& format)
{
    return measureTextLayout(text, area, font, format, false).bounds;
}

}
