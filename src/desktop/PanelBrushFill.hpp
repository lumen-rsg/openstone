#pragma once

#include "yellow/Value.hpp"

#include <QPainterPath>
#include <QPointF>
#include <QQuickItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

Q_DECLARE_METATYPE(openstone::yellow::PaintTool)

class PanelBrushFill : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantMap drawing MEMBER drawing_ NOTIFY drawingChanged FINAL)
    Q_PROPERTY(QPointF viewOrigin MEMBER viewOrigin_ NOTIFY viewOriginChanged FINAL)
    Q_PROPERTY(qreal pixelsPerUnit MEMBER pixelsPerUnit_ NOTIFY pixelsPerUnitChanged FINAL)
    Q_PROPERTY(qreal rasterScale MEMBER rasterScale_ NOTIFY rasterScaleChanged FINAL)

public:
    explicit PanelBrushFill(QQuickItem* parent = nullptr);

signals:
    void drawingChanged();
    void viewOriginChanged();
    void pixelsPerUnitChanged();
    void rasterScaleChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;

private:
    QVariantMap drawing_;
    QPointF viewOrigin_;
    qreal pixelsPerUnit_ = 1;
    qreal rasterScale_ = 1;
};
