#include "PanelBrushFill.hpp"
#include "PanelRadialBrush.hpp"

#include <QBrush>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>

namespace {

using namespace openstone::yellow;

// A texture pixel occupies one FlowStone grid pixel at the brush's local scale.
constexpr double brushPixelsPerUnit = 8;

struct BrushTexture : QSGNode {
    PaintTool tool;
    QPainterPath path;
    QRectF coverage;
    QSize size;
};

QRectF brushArea(Area area)
{
    if (!std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.width) || !std::isfinite(area.height)) {
        return {};
    }
    return QRectF(area.x, area.y, area.width, area.height).normalized();
}

QRgb blend(const ColorChannels& first, const ColorChannels& last, double progress)
{
    const auto channel = [&](int index) { return qRound(std::lerp(first[index], last[index], progress)); };
    return qPremultiply(qRgba(channel(1), channel(2), channel(3), channel(0)));
}

double tileCoordinate(double x, double width, bool flip)
{
    const double period = flip ? width * 2 : width;
    auto local = std::fmod(x, period);
    if (local < 0) {
        local += period;
    }
    if (flip && local >= width) {
        return period - local;
    }
    return local < period ? local : 0;
}

bool flipsX(BrushWrap wrap)
{
    return wrap == BrushWrap::FlipX || wrap == BrushWrap::FlipXY;
}

bool flipsY(BrushWrap wrap)
{
    return wrap == BrushWrap::FlipY || wrap == BrushWrap::FlipXY;
}

template <typename Sample>
QImage rasterBrush(const QRectF& coverage, QSize size, Sample sample)
{
    QImage pixels(size, QImage::Format_ARGB32_Premultiplied);
    if (pixels.isNull()) {
        return {};
    }
    const auto dx = coverage.width() / size.width();
    const auto dy = coverage.height() / size.height();
    for (int row = 0; row < size.height(); ++row) {
        auto* scan = reinterpret_cast<QRgb*>(pixels.scanLine(row));
        const auto y = coverage.y() + (row + 0.5) * dy;
        for (int column = 0; column < size.width(); ++column) {
            const auto x = coverage.x() + (column + 0.5) * dx;
            scan[column] = sample(x, y);
        }
    }
    return pixels;
}

QImage rasterize(const LinearGradientBrush& brush, const QRectF& coverage, QSize size)
{
    const auto area = brushArea(brush.area);
    const auto angle = brush.angle * std::numbers::pi / 180;
    const auto dx = std::cos(angle);
    const auto dy = std::sin(angle);
    const auto extent = std::abs(dx) * area.width() + std::abs(dy) * area.height();
    const auto start = colorToArgb(brush.start);
    const auto end = colorToArgb(brush.end);
    return rasterBrush(coverage, size, [&](double x, double y) {
        if (area.isEmpty()) {
            return qPremultiply(brush.start.argb);
        }
        x -= area.x();
        y -= area.y();
        if (brush.wrap != BrushWrap::Clamp) {
            x = tileCoordinate(x, area.width(), flipsX(brush.wrap));
            y = tileCoordinate(y, area.height(), flipsY(brush.wrap));
        }
        const auto offset = (x - area.width() / 2) * dx + (y - area.height() / 2) * dy;
        return blend(start, end, std::clamp(0.5 + offset / extent, 0.0, 1.0));
    });
}

QImage rasterize(const RadialGradientBrush& brush, const QRectF& coverage, QSize size)
{
    return openstone::desktop::rasterizeRadialBrush(brush, coverage, size);
}

bool hatchPixel(HatchStyle style, int x, int y)
{
    using enum HatchStyle;
    if (style >= Percent05 && style <= Percent90) {
        constexpr std::array coverage{3, 6, 13, 16, 19, 26, 32, 38, 45, 48, 51, 58};
        constexpr int bayer[][8]{
            {0, 48, 12, 60, 3, 51, 15, 63}, {32, 16, 44, 28, 35, 19, 47, 31},
            {8, 56, 4, 52, 11, 59, 7, 55}, {40, 24, 36, 20, 43, 27, 39, 23},
            {2, 50, 14, 62, 1, 49, 13, 61}, {34, 18, 46, 30, 33, 17, 45, 29},
            {10, 58, 6, 54, 9, 57, 5, 53}, {42, 26, 38, 22, 41, 25, 37, 21},
        };
        return bayer[y][x] < coverage[static_cast<unsigned>(style) - static_cast<unsigned>(Percent05)];
    }
    const int down = (x - y + 8) % 8;
    const int up = (x + y) % 8;
    const int diamond = std::abs(x - 3) + std::abs(y - 3);
    switch (style) {
    case Horizontal: return y == 0;
    case Vertical: return x == 0;
    case ForwardDiagonal: return down == 0;
    case BackwardDiagonal: return up == 0;
    case Cross: return x == 0 || y == 0;
    case DiagonalCross: return down == 0 || up == 0;
    case LightDownwardDiagonal: return down % 4 == 0;
    case LightUpwardDiagonal: return up % 4 == 0;
    case DarkDownwardDiagonal: return down % 4 < 2;
    case DarkUpwardDiagonal: return up % 4 < 2;
    case WideDownwardDiagonal: return down < 3;
    case WideUpwardDiagonal: return up < 3;
    case LightVertical: return x % 4 == 0;
    case LightHorizontal: return y % 4 == 0;
    case NarrowVertical: return x % 2 == 0;
    case NarrowHorizontal: return y % 2 == 0;
    case DarkVertical: return x % 4 < 2;
    case DarkHorizontal: return y % 4 < 2;
    case DashedDownwardDiagonal: return (down == 0 && x < 4) || (down == 4 && x >= 4);
    case DashedUpwardDiagonal: return (up == 0 && x < 4) || (up == 4 && x >= 4);
    case DashedHorizontal: return y % 4 == 0 && ((x + y) % 8 < 4);
    case DashedVertical: return x % 4 == 0 && ((x + y) % 8 < 4);
    case SmallConfetti: return (x + 3 * y) % 11 == 0 || (x + 5 * y) % 19 == 0;
    case LargeConfetti: return (x / 2 + 2 * (y / 2)) % 5 < 2;
    case ZigZag: return y % 4 == std::min(x, 7 - x);
    case Wave: {
        constexpr int wave[]{1, 0, 0, 1, 2, 3, 3, 2};
        return y % 4 == wave[x];
    }
    case DiagonalBrick: return down == 0 || (up % 4 == 0 && down < 4);
    case HorizontalBrick: return y % 4 == 0 || (x + (y / 4) * 4) % 8 == 0;
    case Weave: return (down < 2 && (x < 4) == (y < 4)) || (up < 2 && (x < 4) != (y < 4));
    case Plaid: return (x < 4 && y < 4) || ((x < 4 || y < 4) && (x + y) % 2 == 0);
    case Divot: return (x == 1 && y < 3) || (y == 2 && x < 3) || (x == 5 && y >= 5) || (y == 6 && x >= 4 && x <= 6);
    case DottedGrid: return (x % 4 == 0 && y % 2 == 0) || (y % 4 == 0 && x % 2 == 0);
    case DottedDiamond: return diamond == 3 && x % 2 == 0;
    case Shingle: return y == 1 || (y == 5 && x >= 4) || (y >= 2 && y <= 5 && x == y - 1);
    case Trellis: return down % 4 < 2 || up % 4 < 2;
    case Sphere: return (x - 3) * (x - 3) + (y - 3) * (y - 3) <= 9 || (x == 7 && y == 7);
    case SmallGrid: return x % 4 == 0 || y % 4 == 0;
    case SmallCheckerBoard: return ((x / 2) + (y / 2)) % 2 == 0;
    case LargeCheckerBoard: return (x < 4) == (y < 4);
    case OutlinedDiamond: return diamond == 3;
    case SolidDiamond: return diamond <= 3;
    default: return false;
    }
}

QImage rasterize(const HatchBrush& brush, const QRectF& coverage, QSize size)
{
    std::array<std::array<QRgb, 8>, 8> tile{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            tile[y][x] = qPremultiply(hatchPixel(brush.style, x, y) ? brush.foreground.argb : brush.background.argb);
        }
    }
    return rasterBrush(coverage, size, [&](double x, double y) {
        const auto u = static_cast<int>(tileCoordinate(x * brushPixelsPerUnit, 8, false));
        const auto v = static_cast<int>(tileCoordinate(y * brushPixelsPerUnit, 8, false));
        return tile[v][u];
    });
}

QImage rasterize(const TextureBrush& brush, const QRectF& coverage, QSize size)
{
    if (brush.bitmap.empty()) {
        return {};
    }
    const auto pixels = brush.bitmap.rgba();
    const auto width = brush.bitmap.width();
    const auto height = brush.bitmap.height();
    const auto pixel = [&](double coordinate, int extent, bool flip) {
        coordinate *= brushPixelsPerUnit;
        if (brush.wrap != BrushWrap::Clamp) {
            coordinate = tileCoordinate(coordinate, extent, flip);
        }
        return static_cast<int>(std::clamp(coordinate, 0.0, static_cast<double>(extent - 1)));
    };
    return rasterBrush(coverage, size, [&](double x, double y) {
        const auto u = pixel(x, width, flipsX(brush.wrap));
        const auto v = pixel(y, height, flipsY(brush.wrap));
        const auto i = (static_cast<std::size_t>(v) * width + u) * 4;
        return qPremultiply(qRgba(pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]));
    });
}

QImage brushImage(const PaintTool& tool, const QRectF& coverage, QSize size)
{
    return std::visit([&](const auto& brush) -> QImage {
        if constexpr (requires { rasterize(brush, coverage, size); }) {
            return rasterize(brush, coverage, size);
        } else {
            return {};
        }
    }, tool);
}

}

PanelBrushFill::PanelBrushFill(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    connect(this, &PanelBrushFill::drawingChanged, this, &QQuickItem::update);
    connect(this, &PanelBrushFill::viewOriginChanged, this, &QQuickItem::update);
    connect(this, &PanelBrushFill::pixelsPerUnitChanged, this, &QQuickItem::update);
    connect(this, &PanelBrushFill::rasterScaleChanged, this, &QQuickItem::update);
}

void PanelBrushFill::geometryChange(const QRectF& next, const QRectF& previous)
{
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        update();
    }
}

QSGNode* PanelBrushFill::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    std::unique_ptr<BrushTexture> cached(static_cast<BrushTexture*>(old));
    if (width() <= 0 || height() <= 0 || pixelsPerUnit_ <= 0 || rasterScale_ <= 0) {
        return nullptr;
    }
    const auto path = drawing_.value("brushPath").value<QPainterPath>();
    if (path.isEmpty()) {
        return nullptr;
    }

    const auto tool = drawing_.value("brush").value<PaintTool>();
    const QRectF coverage(viewOrigin_, QSizeF(width(), height()) / pixelsPerUnit_);
    const auto scale = std::min(rasterScale_, 4096 / std::max(width(), height()));
    const QSize textureSize(std::max(1, qCeil(width() * scale)), std::max(1, qCeil(height() * scale)));
    if (!cached || cached->tool != tool || cached->path != path || cached->coverage != coverage || cached->size != textureSize) {
        const auto fill = brushImage(tool, coverage, textureSize);
        if (fill.isNull()) {
            return nullptr;
        }
        QImage image(textureSize, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            return nullptr;
        }
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QTransform toPixels;
        toPixels.scale(textureSize.width() / coverage.width(), textureSize.height() / coverage.height());
        toPixels.translate(-coverage.x(), -coverage.y());
        painter.fillPath(toPixels.map(path), QBrush(fill));
        painter.end();

        auto texture = std::unique_ptr<QSGTexture>(window()->createTextureFromImage(image));
        if (!texture) {
            return nullptr;
        }
        cached = std::make_unique<BrushTexture>();
        cached->tool = tool;
        cached->path = path;
        cached->coverage = coverage;
        cached->size = textureSize;
        auto* imageNode = window()->createImageNode();
        imageNode->setTexture(texture.release());
        imageNode->setOwnsTexture(true);
        imageNode->setFiltering(QSGTexture::Linear);
        cached->appendChildNode(imageNode);
    }
    static_cast<QSGImageNode*>(cached->firstChild())->setRect(0, 0, width(), height());
    return cached.release();
}
