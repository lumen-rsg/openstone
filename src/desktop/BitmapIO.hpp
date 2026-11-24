#pragma once

#include "SvgDocument.hpp"

#include <QSize>
#include <QString>
#include <QVariantMap>

class QQmlEngine;

namespace openstone::desktop {

yellow::Bitmap readBitmapFile(const QString& path);
yellow::Bitmap readBitmapData(const QByteArray& data);
SvgDocument readSvgFile(const QString& path);
void saveBitmapFile(const QString& path, QString encoding, const yellow::Bitmap& bitmap);
yellow::Bitmap renderBitmapSurface(QQmlEngine& engine, QSize size, const yellow::Bitmap& background,
                                   const QVariantMap& properties);

}
