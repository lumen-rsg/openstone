#include "PanelText.hpp"
#include "PanelTextLayout.hpp"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGTextNode>
#include <QTextLayout>

#include <cmath>

PanelText::PanelText(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setClip(true);
    connect(this, &PanelText::contentsChanged, this, [this] {
        setClip(drawing_.value("clipText", true).toBool());
        update();
    });
}

void PanelText::geometryChange(const QRectF& next, const QRectF& previous)
{
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        update();
    }
}

QSGNode* PanelText::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    if (drawing_.value("text").toString().isEmpty() || width() <= 0 || height() <= 0 ||
        !std::isfinite(pixelsPerUnit_) || pixelsPerUnit_ <= 0) {
        delete old;
        return nullptr;
    }
    using namespace openstone::desktop;
    // A point has no layout extent. Center and far offsets are relative to the anchor itself.
    const QSizeF extent = drawing_.value("pointOrigin").toBool() ? QSizeF(0, 0)
                                                                : size() * (layoutPixelsPerUnit / pixelsPerUnit_);
    QTextLayout layout;
    const auto lineCount = layoutPanelText(layout, drawing_, extent);
    if (lineCount == 0) {
        delete old;
        return nullptr;
    }

    auto* node = old ? static_cast<QSGTextNode*>(old) : window()->createTextNode();
    node->clear();
    const auto hint = openstone::yellow::TextRenderingHint(drawing_.value("textRenderingHint").toInt());
    using H = openstone::yellow::TextRenderingHint;
    const bool native = hint == H::SingleBitPerPixelGridFit || hint == H::SingleBitPerPixel || hint == H::ClearTypeGridFit;
    node->setRenderType(native ? QSGTextNode::NativeRendering : QSGTextNode::CurveRendering);
    node->setColor(drawing_.value("color").value<QColor>());
    node->setMatrix(QMatrix4x4(textTransform(drawing_.value("vertical").toBool(), extent.width(),
                                             pixelsPerUnit_ / layoutPixelsPerUnit)));
    node->addTextLayout({}, &layout, -1, -1, 0, lineCount);
    return node;
}
