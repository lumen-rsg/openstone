#pragma once

#include <QQuickItem>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class PanelText : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantMap drawing MEMBER drawing_ NOTIFY contentsChanged FINAL)
    Q_PROPERTY(qreal pixelsPerUnit MEMBER pixelsPerUnit_ NOTIFY contentsChanged FINAL)

public:
    explicit PanelText(QQuickItem* parent = nullptr);

signals:
    void contentsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;

private:
    QVariantMap drawing_;
    qreal pixelsPerUnit_ = 1;
};
