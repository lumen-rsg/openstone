#include "HttpTransfer.hpp"
#include "BitmapIO.hpp"

#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTemporaryFile>

#include <memory>
#include <stdexcept>

namespace openstone::desktop {
namespace {

QString text(std::string_view value)
{
    return QString::fromUtf8(value.data(), value.size());
}

struct ResponseBody {
    explicit ResponseBody(const HttpRequest& request)
    {
        if (request.kind != HttpResponseKind::File) return;
        destination = QFileInfo(text(request.destination)).absoluteFilePath();
        if (request.overwrite) {
            replacement = std::make_unique<QSaveFile>(destination);
            if (!replacement->open(QIODevice::WriteOnly)) fail(replacement->errorString());
        } else {
            if (QFileInfo::exists(destination)) throw std::runtime_error("The destination file already exists");
            const auto directory = QFileInfo(destination).absolutePath();
            newFile = std::make_unique<QTemporaryFile>(directory + QStringLiteral("/.openstone-download.XXXXXX"));
            if (!newFile->open()) fail(newFile->errorString());
        }
    }

    void append(QNetworkReply& reply)
    {
        auto chunk = reply.readAll();
        if (auto* file = replacement ? static_cast<QFileDevice*>(replacement.get()) : newFile.get()) {
            if (file->write(chunk) != chunk.size()) fail(file->errorString());
            return;
        }

        constexpr qsizetype memoryLimit = 64 * 1024 * 1024;
        if (chunk.size() > memoryLimit - bytes.size()) throw std::runtime_error("HTTP response exceeds 64 MiB");
        bytes += chunk;
    }

    HttpResponse finish(HttpResponseKind kind)
    {
        if (replacement && !replacement->commit()) fail(replacement->errorString());
        if (newFile) {
            if (!newFile->flush() || !newFile->rename(destination)) fail(newFile->errorString());
            newFile->setAutoRemove(false);
        }
        if (kind == HttpResponseKind::Image) return {.image = readBitmapData(bytes)};
        if (kind == HttpResponseKind::Text) return {.text = QString::fromUtf8(bytes).toStdString()};
        return {};
    }

    [[noreturn]] static void fail(const QString& error) { throw std::runtime_error(error.toStdString()); }

    QString destination;
    std::unique_ptr<QSaveFile> replacement;
    std::unique_ptr<QTemporaryFile> newFile;
    QByteArray bytes;
    std::string error;
};

QByteArray encodeParameters(const HttpRequest& request)
{
    QByteArray encoded;
    for (std::size_t i = 0; i < request.names.size(); ++i) {
        if (i) encoded += '&';
        encoded += QUrl::toPercentEncoding(text(request.names[i]));
        encoded += '=';
        encoded += QUrl::toPercentEncoding(text(request.values[i]));
    }
    return encoded;
}

}

void sendHttpRequest(QNetworkAccessManager& network, HttpRequest request,
                     std::function<bool(const HttpRequest&)> pending,
                     std::function<void(const HttpRequest&, HttpResponse)> complete)
{
    const QUrl url(text(request.url), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) {
        throw std::invalid_argument("Enter a valid HTTP or HTTPS URL");
    }

    auto body = std::make_shared<ResponseBody>(request);
    QNetworkRequest outbound(url);
    outbound.setTransferTimeout(30000);
    outbound.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply;
    if (request.names.empty()) {
        reply = network.get(outbound);
    } else {
        outbound.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        reply = network.post(outbound, encodeParameters(request));
    }
    reply->setReadBufferSize(256 * 1024);

    QObject::connect(reply, &QNetworkReply::readyRead, &network, [reply, body, request, pending] {
        if (!pending(request)) {
            reply->abort();
            return;
        }
        try {
            body->append(*reply);
        } catch (const std::exception& error) {
            body->error = error.what();
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &network,
                     [reply, body, request = std::move(request), pending = std::move(pending), complete = std::move(complete)] {
        reply->deleteLater();
        if (!pending(request)) return;

        HttpResponse response;
        try {
            if (!body->error.empty()) throw std::runtime_error(body->error);
            if (reply->error() != QNetworkReply::NoError) ResponseBody::fail(reply->errorString());
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status < 200 || status >= 300) throw std::runtime_error("HTTP status " + std::to_string(status));
            body->append(*reply);
            response = body->finish(request.kind);
        } catch (const std::exception& error) {
            response.error = error.what();
        }
        complete(request, std::move(response));
    });
}

}
