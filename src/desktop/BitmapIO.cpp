#include "BitmapIO.hpp"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QImageWriter>
#include <QPainter>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <rhi/qrhi.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace openstone::desktop {
namespace {

using yellow::Bitmap;

QImage bitmapImage(const Bitmap& bitmap)
{
    return {bitmap.rgba().data(), bitmap.width(), bitmap.height(),
            static_cast<qsizetype>(bitmap.width()) * 4, QImage::Format_RGBA8888};
}

Bitmap toBitmap(const QImage& decoded)
{
    const auto image = decoded.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        throw std::runtime_error("Could not convert the bitmap to RGBA");
    }
    const auto rowBytes = static_cast<std::size_t>(image.width()) * 4;
    std::vector<std::uint8_t> rgba(rowBytes * image.height());
    for (int row = 0; row < image.height(); ++row) {
        std::copy_n(image.constScanLine(row), rowBytes, rgba.data() + row * rowBytes);
    }
    return {image.width(), image.height(), std::move(rgba)};
}

QImage softwareSurface(QQuickWindow& window, QQuickRenderControl& control, QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        throw std::runtime_error("Could not allocate the bitmap surface");
    }
    image.fill(Qt::transparent);
    window.setRenderTarget(QQuickRenderTarget::fromPaintDevice(&image));
    control.polishItems();
    control.sync();
    control.render();
    window.setRenderTarget({});
    return image;
}

QImage acceleratedSurface(QQuickWindow& window, QQuickRenderControl& control, QSize size)
{
    if (!control.initialize()) {
        throw std::runtime_error("Could not initialize bitmap rendering");
    }
    auto& rhi = *control.rhi();
    const auto extent = rhi.resourceLimit(QRhi::TextureSizeMax);
    if (std::max(size.width(), size.height()) > extent) {
        throw std::runtime_error("Bitmap exceeds the GPU texture size limit");
    }

    const std::unique_ptr<QRhiTexture> texture(rhi.newTexture(QRhiTexture::RGBA8, size, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    const std::unique_ptr<QRhiRenderBuffer> depth(rhi.newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
    if (!texture->create() || !depth->create()) {
        throw std::runtime_error("Could not allocate a bitmap render target");
    }
    const std::unique_ptr<QRhiTextureRenderTarget> target(
        rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(QRhiColorAttachment(texture.get()), depth.get())));
    const std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    if (!target->create()) {
        throw std::runtime_error("Could not create a bitmap render target");
    }

    window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));
    const auto detach = qScopeGuard([&] {
        window.setRenderTarget({});
        control.invalidate();
    });
    control.polishItems();
    control.beginFrame();
    auto* commands = control.commandBuffer();
    if (!commands) {
        throw std::runtime_error("Could not begin bitmap rendering");
    }
    control.sync();
    control.render();
    QRhiReadbackResult result;
    auto* batch = rhi.nextResourceUpdateBatch();
    batch->readBackTexture(texture.get(), &result);
    commands->resourceUpdate(batch);
    // The offscreen frame has completed by the time endFrame returns.
    control.endFrame();
    if (result.pixelSize != size || result.data.size() != qsizetype(size.width()) * size.height() * 4) {
        throw std::runtime_error("Could not read bitmap pixels from the render target");
    }
    const QImage pixels(reinterpret_cast<const uchar*>(result.data.constData()), size.width(), size.height(),
                        QImage::Format_RGBA8888_Premultiplied);
    if (rhi.isYUpInFramebuffer()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        return pixels.flipped(Qt::Vertical);
#else
        return pixels.mirrored(false, true);
#endif
    }
    return pixels.copy();
}

}

static Bitmap readBitmap(QIODevice& input)
{
    QImageReader reader(&input);
    reader.setAutoTransform(true);
    const auto decoded = reader.read();
    if (decoded.isNull()) {
        throw std::runtime_error(QStringLiteral("Could not read the bitmap: %1").arg(reader.errorString()).toStdString());
    }
    return toBitmap(decoded);
}

Bitmap readBitmapFile(const QString& path)
{
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Could not open the bitmap file");
    }
    return readBitmap(file);
}

Bitmap readBitmapData(const QByteArray& data)
{
    QBuffer input;
    input.setData(data);
    input.open(QIODevice::ReadOnly);
    return readBitmap(input);
}

SvgDocument readSvgFile(const QString& path)
{
    QFile file(path);
    if (path.contains(QChar::Null) || !QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Could not open the SVG file");
    }

    // Inspect the root viewport before passing the stream to Qt so fractional pixel sizes survive.
    const auto bytes = file.readAll();
    QXmlStreamReader header(bytes);
    if (!header.readNextStartElement() || header.name() != u"svg") {
        throw std::runtime_error("Could not read the SVG file");
    }
    const auto attributes = header.attributes();
    auto renderer = std::make_shared<QSvgRenderer>();
    renderer->setAnimationEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    renderer->setOptions(renderer->options() | QtSvg::DisableAnimations);
#endif
    QXmlStreamReader xml(bytes);
    const bool loaded = renderer->load(&xml);
    while (!xml.atEnd()) {
        xml.readNext();
    }
    if (!loaded || xml.hasError()) {
        throw std::runtime_error("Could not parse the SVG file");
    }
    renderer->setAspectRatioMode(attributes.value(u"preserveAspectRatio").trimmed() == u"none"
        ? Qt::IgnoreAspectRatio : Qt::KeepAspectRatio);

    const auto intrinsic = [](QStringView length, double viewBox, double fallback) {
        length = length.trimmed();
        if (length.isEmpty() || length.endsWith(u'%')) {
            return viewBox > 0 ? viewBox : fallback;
        }
        if (length.endsWith(u"px")) {
            length.chop(2);
        }
        bool ok = false;
        const auto pixels = length.toDouble(&ok);
        return ok ? pixels : fallback;
    };
    const auto natural = renderer->defaultSize();
    const auto viewBox = renderer->viewBoxF();
    const double width = intrinsic(attributes.value(u"width"), viewBox.width(), natural.width());
    const double height = intrinsic(attributes.value(u"height"), viewBox.height(), natural.height());

    return {width, height, [renderer = std::move(renderer), width, height](const SvgRasterLayout& layout) {
        QImage image(layout.width, layout.height, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) {
            throw std::runtime_error("Could not allocate the SVG bitmap");
        }
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(layout.x, layout.y);
        painter.scale(layout.scaleX, layout.scaleY);
        renderer->render(&painter, QRectF(0, 0, width, height));
        painter.end();
        return toBitmap(image);
    }};
}

void saveBitmapFile(const QString& path, QString encoding, const Bitmap& bitmap)
{
    if (path.contains(QChar::Null)) {
        throw std::invalid_argument("Invalid Bitmap Save path");
    }
    encoding = encoding.trimmed().toLower();
    if (encoding.isEmpty()) {
        encoding = QFileInfo(path).suffix().toLower();
        if (encoding.isEmpty()) {
            encoding = QStringLiteral("png");
        }
    }
    const auto format = encoding.toLatin1();
    if (!QImageWriter::supportedImageFormats().contains(format)) {
        throw std::invalid_argument(QStringLiteral("No bitmap encoder for %1").arg(encoding).toStdString());
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(QStringLiteral("Could not open bitmap destination: %1").arg(file.errorString()).toStdString());
    }
    QImageWriter writer(&file, format);
    if (!writer.write(bitmapImage(bitmap))) {
        throw std::runtime_error(QStringLiteral("Could not write the bitmap: %1").arg(writer.errorString()).toStdString());
    }
    if (!file.commit()) {
        throw std::runtime_error(QStringLiteral("Could not save the bitmap: %1").arg(file.errorString()).toStdString());
    }
}

Bitmap renderBitmapSurface(QQmlEngine& engine, QSize size, const Bitmap& background,
                           const QVariantMap& properties)
{
    const auto panel = properties.value("panel").toMap();
    if (panel.value("drawings").toList().isEmpty() && panel.value("items").toList().isEmpty()) {
        return background.empty()
            ? Bitmap{size.width(), size.height(), std::vector<std::uint8_t>(std::size_t(size.width()) * size.height() * 4)}
            : background;
    }

    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.resize(size);
    window.setColor(Qt::transparent);

    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/OpenStone/FrontPanel.qml")));
    auto surface = std::unique_ptr<QObject>(component.createWithInitialProperties(properties));
    auto* item = qobject_cast<QQuickItem*>(surface.get());
    if (!item) {
        throw std::runtime_error(QStringLiteral("Could not create the bitmap View: %1").arg(component.errorString()).toStdString());
    }
    item->setSize(size);
    item->setParentItem(window.contentItem());

    auto overlay = window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software
        ? softwareSurface(window, control, size) : acceleratedSurface(window, control, size);
    if (background.empty()) {
        return toBitmap(overlay);
    }
    auto image = bitmapImage(background).copy();
    if (image.isNull()) {
        throw std::runtime_error("Could not copy the source bitmap");
    }
    QPainter painter(&image);
    painter.drawImage(0, 0, overlay);
    painter.end();
    return toBitmap(image);
}

}
