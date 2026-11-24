#pragma once

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class CanvasGrid : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QPointF center MEMBER center_ NOTIFY gridChanged FINAL)
    Q_PROPERTY(double zoom MEMBER zoom_ NOTIFY gridChanged FINAL)
    Q_PROPERTY(QColor dotColor MEMBER dotColor_ NOTIFY gridChanged FINAL)

public:
    explicit CanvasGrid(QQuickItem* parent = nullptr);

signals:
    void gridChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;

private:
    QPointF center_;
    double zoom_ = 1;
    QColor dotColor_ = Qt::gray;
};
