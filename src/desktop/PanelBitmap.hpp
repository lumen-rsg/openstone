#pragma once

#include "yellow/Value.hpp"

#include <QQuickItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

Q_DECLARE_METATYPE(openstone::yellow::Bitmap)
Q_DECLARE_METATYPE(openstone::yellow::ColorMatrix)

class PanelBitmap : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantMap drawing MEMBER drawing_ NOTIFY drawingChanged FINAL)

public:
    explicit PanelBitmap(QQuickItem* parent = nullptr);

signals:
    void drawingChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;

private:
    QVariantMap drawing_;
};
