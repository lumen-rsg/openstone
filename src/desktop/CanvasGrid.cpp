#include "CanvasGrid.hpp"

#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>

#include <algorithm>
#include <cmath>

CanvasGrid::CanvasGrid(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    connect(this, &CanvasGrid::gridChanged, this, &QQuickItem::update);
}

void CanvasGrid::geometryChange(const QRectF& next, const QRectF& previous)
{
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        update();
    }
}

QSGNode* CanvasGrid::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    if (width() <= 0 || height() <= 0 || !std::isfinite(zoom_) || zoom_ <= 0) {
        delete old;
        return nullptr;
    }

    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);
        node->setMaterial(new QSGFlatColorMaterial);
        node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    }

    // Skip finer subdivisions at low zoom to keep vertex counts bounded.
    double spacing = 24 * zoom_;
    while (spacing < 16) {
        spacing *= 2;
    }
    const auto first = [spacing](double offset) {
        const double remainder = std::fmod(offset, spacing);
        return remainder < 0 ? remainder + spacing : remainder;
    };

    const double left = first(width() / 2 - center_.x() * zoom_);
    const double top = first(height() / 2 - center_.y() * zoom_);
    const int columns = std::max(0, static_cast<int>(std::ceil((width() - left) / spacing)));
    const int rows = std::max(0, static_cast<int>(std::ceil((height() - top) / spacing)));
    auto* geometry = node->geometry();
    const int count = columns * rows * 6;
    if (geometry->vertexCount() != count) {
        geometry->allocate(count);
    }

    auto* vertex = geometry->vertexDataAsPoint2D();
    constexpr float radius = 0.8F;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const auto x = static_cast<float>(left + column * spacing);
            const auto y = static_cast<float>(top + row * spacing);
            (vertex++)->set(x - radius, y - radius);
            (vertex++)->set(x + radius, y - radius);
            (vertex++)->set(x + radius, y + radius);
            (vertex++)->set(x - radius, y - radius);
            (vertex++)->set(x + radius, y + radius);
            (vertex++)->set(x - radius, y + radius);
        }
    }
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);

    auto* material = static_cast<QSGFlatColorMaterial*>(node->material());
    if (material->color() != dotColor_) {
        material->setColor(dotColor_);
        node->markDirty(QSGNode::DirtyMaterial);
    }
    return node;
}
