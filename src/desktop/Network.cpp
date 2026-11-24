#include "Network.hpp"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <algorithm>
#include <stdexcept>

namespace openstone::desktop {
namespace {

class QtNetworkConnection final : public NetworkConnection {
public:
    explicit QtNetworkConnection(const NetworkSettings& settings) : server_(settings.server)
    {
        if (settings.datagram) {
            auto udp = std::make_unique<QUdpSocket>();
            if (server_) {
                if (!udp->bind(QHostAddress::Any, settings.port)) fail(udp->errorString());
            } else {
                udp->connectToHost(QString::fromStdString(settings.host), settings.port);
            }
            socket_ = std::move(udp);
        } else if (server_) {
            listener_ = std::make_unique<QTcpServer>();
            if (!listener_->listen(QHostAddress::Any, settings.port)) fail(listener_->errorString());
        } else {
            socket_ = std::make_unique<QTcpSocket>();
            socket_->connectToHost(QString::fromStdString(settings.host), settings.port);
        }
        if (server_) {
            const auto addresses = QNetworkInterface::allAddresses();
            const auto address = std::ranges::find_if(addresses, [](const QHostAddress& ip) {
                return ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback();
            });
            address_ = address == addresses.end() ? "127.0.0.1" : address->toString().toStdString();
        }
    }

    NetworkPoll poll() override
    {
        NetworkPoll result{.status = "Open", .address = address_};
        if (server_ && listener_) {
            while (listener_->hasPendingConnections()) {
                if (auto* client = listener_->nextPendingConnection()) clients_.push_back(client);
            }
            if (!listener_->isListening()) {
                result.status = listener_->errorString().toStdString();
                result.finished = true;
            }
            for (auto& client : clients_) {
                if (!client) continue;
                readBytes(*client, result);
                if (closedAndDrained(*client)) client->deleteLater();
            }
            std::erase_if(clients_, [](const auto& client) {
                return !client || closedAndDrained(*client);
            });
        } else if (auto* udp = qobject_cast<QUdpSocket*>(socket_.get())) {
            for (int count = 0; count < 64 && udp->hasPendingDatagrams(); ++count) {
                auto packet = udp->receiveDatagram();
                if (!packet.isValid()) break;
                if (server_) {
                    const auto peer = std::pair{packet.senderAddress(), static_cast<quint16>(packet.senderPort())};
                    if (peers_.size() < 256 && !std::ranges::contains(peers_, peer)) peers_.push_back(peer);
                }
                result.received.push_back(packet.data().toStdString());
            }
        } else if (socket_) {
            readBytes(*socket_, result);
        }
        if (!server_) {
            if (socket_->state() == QAbstractSocket::UnconnectedState) {
                const auto error = socket_->error();
                result.status = error == QAbstractSocket::UnknownSocketError || error == QAbstractSocket::RemoteHostClosedError
                    ? "Closed" : socket_->errorString().toStdString();
                result.finished = closedAndDrained(*socket_);
            } else if (socket_->state() != QAbstractSocket::ConnectedState) {
                result.status = "Opening";
            }
        }
        return result;
    }

    bool send(std::string_view bytes) override
    {
        if (bytes.size() > 1024 * 1024) return false;
        const QByteArray payload(bytes.data(), static_cast<qsizetype>(bytes.size()));
        if (!server_) return sendTo(*socket_, payload);
        bool sent = false;
        if (auto* udp = qobject_cast<QUdpSocket*>(socket_.get())) {
            for (const auto& [ip, port] : peers_) sent |= udp->writeDatagram(payload, ip, port) == payload.size();
        } else {
            for (auto& client : clients_) if (client) sent |= sendTo(*client, payload);
        }
        return sent;
    }

private:
    [[noreturn]] static void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

    static void readBytes(QAbstractSocket& socket, NetworkPoll& result)
    {
        if (socket.bytesAvailable() > 0) result.received.push_back(socket.read(64 * 1024).toStdString());
    }

    static bool closedAndDrained(QAbstractSocket& socket)
    {
        return socket.state() == QAbstractSocket::UnconnectedState && socket.bytesAvailable() == 0;
    }

    static bool sendTo(QAbstractSocket& socket, const QByteArray& payload)
    {
        return socket.state() == QAbstractSocket::ConnectedState && socket.bytesToWrite() < 1024 * 1024 &&
               socket.write(payload) == payload.size();
    }

    bool server_;
    std::string address_;
    std::unique_ptr<QAbstractSocket> socket_;
    std::unique_ptr<QTcpServer> listener_;
    std::vector<QPointer<QTcpSocket>> clients_;
    std::vector<std::pair<QHostAddress, quint16>> peers_;
};

}

std::shared_ptr<NetworkConnection> openNetwork(const NetworkSettings& settings)
{
    return std::make_shared<QtNetworkConnection>(settings);
}

}
