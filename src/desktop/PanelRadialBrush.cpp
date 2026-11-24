#include "PanelRadialBrush.hpp"

#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace openstone::desktop {
namespace {
using namespace yellow;
using Channels = std::array<double, 4>;
Channels channels(Color color) {
    const auto c = colorToArgb(color);
    return {double(c[0]), double(c[1]), double(c[2]), double(c[3])};
}
Channels mix(Channels a, Channels b, double t) {
    for (int i = 0; i < 4; ++i)
        a[i] = std::lerp(a[i], b[i], t);
    return a;
}
QRgb pixel(Channels c) { return qPremultiply(qRgba(qRound(c[1]), qRound(c[2]), qRound(c[3]), qRound(c[0]))); }
QPointF qt(green::Point p) { return {p.x, p.y}; }
double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
struct Vertex {
    QPointF point;
    Channels color;
};
struct Edge {
    Vertex a, b;
};

std::vector<Edge> boundaryEdges(const RadialGradientBrush &brush, double tolerance) {
    const auto &path = *brush.boundary;
    const auto vertex = [&](std::size_t i) {
        const auto color = brush.surroundColors.empty()
                               ? brush.end
                               : brush.surroundColors[std::min(i, brush.surroundColors.size() - 1)];
        return Vertex{qt(path.points[i]), channels(color)};
    };
    std::vector<Edge> edges;
    auto first = vertex(0), last = first;
    const auto line = [&](Vertex a, Vertex b) {
        if (a.point != b.point)
            edges.push_back({a, b});
    };
    const auto midpoint = [](Vertex a, Vertex b) {
        return Vertex{(a.point + b.point) * .5, mix(a.color, b.color, .5)};
    };
    const auto cubic = [&](auto &&self, Vertex a, Vertex b, Vertex c, Vertex d, int depth) -> void {
        const auto delta = d.point - a.point;
        const auto distance = [&](QPointF p) {
            return delta.isNull() ? std::hypot(p.x() - a.point.x(), p.y() - a.point.y())
                                  : std::abs(cross(p - a.point, delta)) / std::hypot(delta.x(), delta.y());
        };
        if (depth == 8 || std::max(distance(b.point), distance(c.point)) <= tolerance) {
            line(a, d);
            return;
        }
        const auto ab = midpoint(a, b), bc = midpoint(b, c), cd = midpoint(c, d), abc = midpoint(ab, bc),
                   bcd = midpoint(bc, cd), m = midpoint(abc, bcd);
        self(self, a, ab, abc, m, depth + 1);
        self(self, m, bcd, cd, d, depth + 1);
    };
    if (path.types.empty()) {
        for (std::size_t i = 1; i < path.points.size(); ++i) {
            const auto next = vertex(i);
            line(last, next);
            last = next;
        }
        line(last, first);
        return edges;
    }
    for (std::size_t i = 1; i < path.points.size(); ++i) {
        const auto type = path.types[i] & 7;
        if (type == 0) {
            line(last, first);
            first = last = vertex(i);
        } else if (type == 1) {
            const auto next = vertex(i);
            line(last, next);
            last = next;
        } else {
            const auto b = vertex(i), c = vertex(i + 1), d = vertex(i + 2);
            cubic(cubic, last, b, c, d, 0);
            last = d;
            i += 2;
        }
        if (path.types[i] & 0x80) {
            line(last, first);
            last = first;
        }
    }
    line(last, first);
    return edges;
}

double standardExit(RadialShape shape, QPointF center, QPointF ray) {
    if (shape == RadialShape::Rectangle) {
        const auto edge = [](double origin, double delta) {
            return delta == 0 ? std::numeric_limits<double>::infinity()
                              : (std::copysign(1., delta) - origin) / delta;
        };
        return std::min(edge(center.x(), ray.x()), edge(center.y(), ray.y()));
    }
    const auto length2 = QPointF::dotProduct(ray, ray), along = QPointF::dotProduct(center, ray);
    const auto discriminant = along * along - length2 * (QPointF::dotProduct(center, center) - 1);
    return discriminant < 0 ? 0 : (-along + std::sqrt(discriminant)) / length2;
}
} // namespace

QPainterPath radialBoundaryPath(const yellow::RadialPath &boundary) {
    yellow::validateRadialPath(boundary);
    QPainterPath path;
    path.setFillRule(boundary.winding ? Qt::WindingFill : Qt::OddEvenFill);
    if (boundary.types.empty()) {
        path.moveTo(qt(boundary.points[0]));
        for (std::size_t i = 1; i < boundary.points.size(); ++i)
            path.lineTo(qt(boundary.points[i]));
        path.closeSubpath();
        return path;
    }
    for (std::size_t i = 0; i < boundary.points.size(); ++i) {
        const auto type = boundary.types[i] & 7;
        if (type == 0)
            path.moveTo(qt(boundary.points[i]));
        else if (type == 1)
            path.lineTo(qt(boundary.points[i]));
        else {
            path.cubicTo(qt(boundary.points[i]), qt(boundary.points[i + 1]), qt(boundary.points[i + 2]));
            i += 2;
        }
        if (boundary.types[i] & 0x80)
            path.closeSubpath();
    }
    return path;
}

QImage rasterizeRadialBrush(const yellow::RadialGradientBrush &brush, const QRectF &coverage, QSize size) {
    if (size.isEmpty() || qint64(size.width()) * size.height() > 4096 * 4096 || coverage.isEmpty() ||
        !std::isfinite(coverage.x()) || !std::isfinite(coverage.y()) || !std::isfinite(coverage.width()) ||
        !std::isfinite(coverage.height()))
        return {};
    const QRectF area = QRectF(brush.area.x, brush.area.y, brush.area.width, brush.area.height).normalized();
    if (!std::isfinite(area.x()) || !std::isfinite(area.y()) || !std::isfinite(area.width()) ||
        !std::isfinite(area.height()) || !std::isfinite(brush.focus.x) || !std::isfinite(brush.focus.y) ||
        (brush.center && (!std::isfinite(brush.center->x) || !std::isfinite(brush.center->y))))
        return {};
    if (brush.boundary) {
        try {
            validateRadialPath(*brush.boundary);
        } catch (const std::invalid_argument &) {
            return {};
        }
    }
    QImage pixels(size, QImage::Format_ARGB32_Premultiplied), mask;
    if (pixels.isNull())
        return {};
    std::vector<Edge> edges;
    constexpr int bins = 1024;
    std::array<std::vector<int>, bins> candidates;
    const auto center = brush.center ? qt(*brush.center) : area.center();
    const auto angle = [](QPointF p) { return std::atan2(p.y(), p.x()); };
    const auto bin = [&](double a) {
        return std::clamp(int((a + std::numbers::pi) * bins / (2 * std::numbers::pi)), 0, bins - 1);
    };
    if (brush.boundary) {
        const auto path = radialBoundaryPath(*brush.boundary);
        mask = QImage(size, QImage::Format_ARGB32_Premultiplied);
        if (mask.isNull())
            return {};
        mask.fill(Qt::transparent);
        QPainter painter(&mask);
        QTransform transform;
        transform.scale(size.width() / coverage.width(), size.height() / coverage.height());
        transform.translate(-coverage.x(), -coverage.y());
        painter.fillPath(transform.map(path), Qt::white);
        painter.end();
        edges = boundaryEdges(brush,
                              std::min(coverage.width() / size.width(), coverage.height() / size.height()) * .125);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto a = edges[i].a.point - center, b = edges[i].b.point - center;
            if (a.isNull() || b.isNull() || (std::abs(cross(a, b)) < 1e-12 && QPointF::dotProduct(a, b) <= 0)) {
                for (auto &bucket : candidates)
                    bucket.push_back(i);
            } else {
                const auto begin = bin(angle(a)), end = bin(angle(b)), first = std::min(begin, end),
                           last = std::max(begin, end);
                if (last - first <= bins / 2)
                    for (int j = first; j <= last; ++j)
                        candidates[j].push_back(i);
                else {
                    for (int j = 0; j <= first; ++j)
                        candidates[j].push_back(i);
                    for (int j = last; j < bins; ++j)
                        candidates[j].push_back(i);
                }
            }
        }
    }
    const auto normalize = [&](QPointF p) {
        return QPointF(2 * (p.x() - area.x()) / area.width() - 1, 2 * (p.y() - area.y()) / area.height() - 1);
    };
    const auto start = channels(brush.start), end = channels(brush.end);
    for (int row = 0; row < size.height(); ++row) {
        auto *scan = reinterpret_cast<QRgb *>(pixels.scanLine(row));
        for (int col = 0; col < size.width(); ++col) {
            const QPointF point(coverage.x() + (col + .5) * coverage.width() / size.width(),
                                coverage.y() + (row + .5) * coverage.height() / size.height());
            if (area.isEmpty()) {
                scan[col] = pixel(start);
                continue;
            }
            if (brush.boundary ? qAlpha(mask.pixel(col, row)) == 0
                : brush.shape == RadialShape::Ellipse
                    ? QPointF::dotProduct(normalize(point), normalize(point)) > 1
                    : std::max(std::abs(normalize(point).x()), std::abs(normalize(point).y())) > 1) {
                scan[col] = 0;
                continue;
            }
            const auto ray = point - center;
            if (ray.isNull()) {
                scan[col] = pixel(start);
                continue;
            }
            auto outer = std::numeric_limits<double>::infinity();
            auto edgeColor = end;
            if (brush.boundary) {
                for (const auto index : candidates[bin(angle(ray))]) {
                    const auto &edge = edges[index];
                    const auto delta = edge.b.point - edge.a.point, offset = edge.a.point - center;
                    const auto denominator = cross(ray, delta);
                    if (std::abs(denominator) < 1e-15)
                        continue;
                    const auto t = cross(offset, delta) / denominator, u = cross(offset, ray) / denominator;
                    if (t >= 1 - 1e-9 && t < outer && u >= -1e-9 && u <= 1 + 1e-9) {
                        outer = t;
                        edgeColor = mix(edge.a.color, edge.b.color, std::clamp(u, 0., 1.));
                    }
                }
            } else
                outer = standardExit(brush.shape, normalize(center), normalize(point) - normalize(center));
            if (!std::isfinite(outer) || outer < 1) {
                scan[col] = pixel(edgeColor);
                continue;
            }
            const auto distance = [](double x, float focus) {
                return x == 0 ? 0 : focus == 0 ? std::numeric_limits<double>::infinity() : std::abs(x) / focus;
            };
            const auto fx = distance(2 * ray.x() / area.width(), brush.focus.x),
                       fy = distance(2 * ray.y() / area.height(), brush.focus.y);
            const auto inner = 1. / (brush.boundary || brush.shape == RadialShape::Ellipse ? std::hypot(fx, fy)
                                                                                           : std::max(fx, fy));
            const auto progress =
                inner >= 1 || outer <= inner ? 0 : std::clamp((1 - inner) / (outer - inner), 0., 1.);
            scan[col] = pixel(mix(start, edgeColor, progress));
        }
    }
    return pixels;
}
} // namespace openstone::desktop
