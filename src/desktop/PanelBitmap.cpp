#include "PanelBitmap.hpp"

#include <QImage>
#include <QQuickWindow>
#include <QSGImageNode>

#include <memory>
#include <stdexcept>

namespace {

using openstone::yellow::Bitmap;
using openstone::yellow::ColorMatrix;

struct BitmapTexture : QSGNode {
    Bitmap bitmap;
    ColorMatrix matrix;
};

// QImage owns a reference to the shared pixels until Qt has finished with them.
QImage textureImage(const Bitmap& bitmap, const ColorMatrix& matrix)
{
    auto pixels = std::make_unique<Bitmap>(openstone::yellow::applyBitmapColorMatrix(matrix, bitmap));
    const auto* data = pixels->rgba().data();
    const auto width = pixels->width();
    const auto height = pixels->height();
    return QImage(data, width, height, static_cast<qsizetype>(width) * 4, QImage::Format_RGBA8888,
                  [](void* owner) { delete static_cast<Bitmap*>(owner); }, pixels.release());
}

}

PanelBitmap::PanelBitmap(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    connect(this, &PanelBitmap::drawingChanged, this, &QQuickItem::update);
}

void PanelBitmap::geometryChange(const QRectF& next, const QRectF& previous)
{
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        update();
    }
}

QSGNode* PanelBitmap::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    std::unique_ptr<BitmapTexture> cached(static_cast<BitmapTexture*>(old));
    const auto bitmap = drawing_.value("bitmap").value<Bitmap>();
    if (bitmap.empty() || width() <= 0 || height() <= 0) {
        return nullptr;
    }
    const auto matrix = drawing_.value("matrix").value<ColorMatrix>();
    auto* imageNode = cached ? static_cast<QSGImageNode*>(cached->firstChild()) : nullptr;
    if (!cached || cached->bitmap != bitmap || cached->matrix != matrix) {
        QImage image;
        try {
            image = textureImage(bitmap, matrix);
        } catch (const std::invalid_argument&) {
            return nullptr;
        }
        // Convert after the color transform so it can use RGB from transparent pixels.
        auto texture = std::unique_ptr<QSGTexture>(
            window()->createTextureFromImage(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied)));
        if (!texture) {
            return nullptr;
        }
        cached = std::make_unique<BitmapTexture>();
        cached->bitmap = bitmap;
        cached->matrix = matrix;
        imageNode = window()->createImageNode();
        imageNode->setTexture(texture.release());
        imageNode->setOwnsTexture(true);
        imageNode->setFiltering(QSGTexture::Linear);
        cached->appendChildNode(imageNode);
    }

    const auto source = drawing_.value("source").toRectF();
    // Respect the texture size if the backend downsized the upload to its limit.
    const auto size = imageNode->texture()->textureSize();
    const qreal sx = qreal(size.width()) / bitmap.width();
    const qreal sy = qreal(size.height()) / bitmap.height();
    imageNode->setSourceRect(source.x() * sx, source.y() * sy, source.width() * sx, source.height() * sy);
    imageNode->setRect(0, 0, width(), height());
    return cached.release();
}
