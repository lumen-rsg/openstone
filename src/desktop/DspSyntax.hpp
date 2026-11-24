#pragma once

#include <QPointer>
#include <QQuickTextDocument>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QtQml/qqmlregistration.h>

class DspSyntax : public QSyntaxHighlighter {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged FINAL)
    Q_PROPERTY(bool ruby MEMBER ruby_ NOTIFY rubyChanged FINAL)
    Q_PROPERTY(bool assembler MEMBER assembler_ NOTIFY assemblerChanged FINAL)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT FINAL)

public:
    explicit DspSyntax(QObject* parent = nullptr);
    QQuickTextDocument* textDocument() const { return textDocument_; }
    void setTextDocument(QQuickTextDocument* document);
    QString fontFamily() const;
    Q_INVOKABLE QStringList complete(const QString& source, const QString& prefix) const;

signals:
    void rubyChanged();
    void assemblerChanged();
    void textDocumentChanged();

protected:
    void highlightBlock(const QString& text) override;

private:
    bool ruby_ = false;
    bool assembler_ = false;
    QPointer<QQuickTextDocument> textDocument_;
};
