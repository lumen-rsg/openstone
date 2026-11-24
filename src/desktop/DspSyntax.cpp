#include "DspSyntax.hpp"
#include "stream/DspCode.hpp"

#include <QFontDatabase>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

using openstone::stream::DspWordKind;

const auto& words(bool assembler)
{
    using openstone::stream::DspLanguage;
    const auto load = [](DspLanguage language) {
        QHash<QString, DspWordKind> result;
        for (const auto& [text, kind] : openstone::stream::dspLanguageWords(language)) {
            result.insert(QString::fromUtf8(text.data(), text.size()), kind);
        }
        return result;
    };
    static const auto dsp = load(DspLanguage::Code);
    static const auto assembly = load(DspLanguage::Assembler);
    return assembler ? assembly : dsp;
}

QColor wordColor(DspWordKind kind)
{
    switch (kind) {
    case DspWordKind::Port: return QColor(211, 166, 242);
    case DspWordKind::Type: return QColor(144, 201, 240);
    case DspWordKind::Section: return QColor(211, 166, 242);
    case DspWordKind::Literal: return QColor(241, 193, 142);
    case DspWordKind::Function: return QColor(140, 210, 178);
    case DspWordKind::Register: return QColor(227, 173, 139);
    }
    return {};
}

}

DspSyntax::DspSyntax(QObject* parent) : QSyntaxHighlighter(parent)
{
    connect(this, &DspSyntax::rubyChanged, this, [this] { rehighlight(); });
    connect(this, &DspSyntax::assemblerChanged, this, [this] { rehighlight(); });
}

void DspSyntax::setTextDocument(QQuickTextDocument* document)
{
    if (textDocument_ == document) {
        return;
    }
    if (textDocument_) {
        disconnect(textDocument_, nullptr, this, nullptr);
    }
    textDocument_ = document;
    setDocument(document ? document->textDocument() : nullptr);
    if (document) {
        connect(document, &QQuickTextDocument::textDocumentChanged, this, [this] {
            setDocument(textDocument_->textDocument());
        });
    }
    emit textDocumentChanged();
}

QString DspSyntax::fontFamily() const
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

void DspSyntax::highlightBlock(const QString& text)
{
    if (ruby_) {
        static const QRegularExpression rubyTokens(QStringLiteral(
            R"ruby(("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(#.*)|(@@?\w+)|(\b\d[\d_]*(?:\.[\d_]+)?)|(\b[a-zA-Z_]\w*\b))ruby"));
        static const QSet<QString> rubyKeywords{
            "alias", "and", "begin", "break", "case", "class", "def", "do", "else", "elsif", "end",
            "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo",
            "rescue", "retry", "return", "self", "super", "then", "true", "undef", "unless",
            "until", "when", "while", "yield"
        };
        static const QSet<QString> rubyHostMethods{
            "event", "input", "output", "watch", "time", "scheduleMethod", "clearEvents", "saveState", "loadState",
            "draw", "redraw", "drawRectangle", "drawEllipse", "drawRoundRect", "drawLine", "drawLines", "drawPolygon",
            "drawString", "measureString", "drawBitmap", "getViewSize", "isKeyPressed", "setDashStyle",
            "setAlignment", "setLineAlignment", "setFlags", "init", "mouseLUpCaptured", "mouseRUpCaptured",
            "isInMousePoint", "mouseMove", "mouseMoveCaptured",
            "mouseLDown", "mouseLUp", "mouseLDouble", "mouseRDown", "mouseRUp", "mouseRDouble", "captureMouse", "releaseMouse",
            "mouseCursor", "showCursor", "createEdit", "editChanged", "editFinished",
            "createDropList", "dropListFinished", "messageBox", "messageBoxFinished", "playSound", "schematicLocation"
        };
        auto matches = rubyTokens.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            QColor color;
            if (match.hasCaptured(1)) color = wordColor(DspWordKind::Function);
            else if (match.hasCaptured(2)) color = QColor(155, 160, 157);
            else if (match.hasCaptured(3) || match.hasCaptured(4)) color = wordColor(DspWordKind::Literal);
            else if (rubyKeywords.contains(match.captured(5))) color = wordColor(DspWordKind::Type);
            else if (rubyHostMethods.contains(match.captured(5)))
                color = wordColor(DspWordKind::Port);
            if (color.isValid()) setFormat(match.capturedStart(), match.capturedLength(), color);
        }
        return;
    }
    static const QRegularExpression tokens(
        QStringLiteral(R"((//.*)|(\b0[xX][0-9a-fA-F]+|(?:\b\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)|([a-zA-Z_]\w*)|([+*/%=^&|~!<>?:-]))"));
    auto matches = tokens.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto applyColor = [&](const QColor& color) {
            setFormat(match.capturedStart(), match.capturedLength(), color);
        };
        if (match.hasCaptured(1)) {
            applyColor(QColor(155, 160, 157));
        } else if (match.hasCaptured(2)) {
            applyColor(wordColor(DspWordKind::Literal));
        } else if (match.hasCaptured(3)) {
            const auto& language = words(assembler_);
            const auto name = match.captured(3);
            auto found = language.constFind(name);
            if (assembler_ && found == language.cend()) {
                const auto lower = language.constFind(name.toLower());
                if (lower != language.cend() && (*lower == DspWordKind::Function || *lower == DspWordKind::Register))
                    found = lower;
            }
            if (found != language.cend()) applyColor(wordColor(*found));
        } else {
            applyColor(QColor(181, 183, 218));
        }
    }
}

QStringList DspSyntax::complete(const QString& source, const QString& prefix) const
{
    QStringList candidates;
    const auto& language = words(assembler_);
    for (auto it = language.keyBegin(); it != language.keyEnd(); ++it) {
        if (*it != prefix && it->startsWith(prefix)) {
            candidates.append(*it);
        }
    }
    static const QRegularExpression declaration(
        QStringLiteral(R"((//[^\n]*)|\b([a-zA-Z_]\w*)[ \t\r\n]+([a-zA-Z_]\w*))"));
    auto matches = declaration.globalMatch(source);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto type = language.constFind(match.captured(2));
        const auto name = match.captured(3);
        if (type != language.cend() && (*type == DspWordKind::Port || *type == DspWordKind::Type) &&
            !language.contains(name) && name != prefix && name.startsWith(prefix)) {
            candidates.append(name);
        }
    }
    candidates.removeDuplicates();
    std::ranges::sort(candidates);
    return candidates;
}
