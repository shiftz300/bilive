// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/automation.hpp"
#include <QTcpSocket>
#include <QTimer>

namespace bilive {
Automation::Automation(Action action) : action_(std::move(action))
{
    connect(&server_, &QTcpServer::newConnection, this, [this] {
        while (server_.hasPendingConnections()) {
            auto *socket = server_.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
            socket->setReadBufferSize(8193);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                if (socket->bytesAvailable() > 8192) {
                    socket->abort();
                    return;
                }
                const QByteArray available = socket->peek(8192);
                const auto end = available.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                socket->blockSignals(true);
                const auto headers = available.left(end).split('\n');
                const auto line = headers.value(0).trimmed().split(' ');
                QByteArray authorization;
                for (auto header : headers) {
                    const auto colon = header.indexOf(':');
                    if (colon >= 0 && header.left(colon).trimmed().toLower() == "authorization")
                        authorization = header.mid(colon + 1).trimmed();
                }
                QByteArray status;
                const bool start = line.value(1) == "/api/startLive";
                const bool stop = line.value(1) == "/api/stopLive";
                if (line.value(0) != "POST" || (!start && !stop))
                    status = "404 Not Found";
                else if (token_.isEmpty() || authorization != "Bearer " + token_)
                    status = "401 Unauthorized";
                else
                    status = action_(start) ? "202 Accepted" : "409 Conflict";
                socket->blockSignals(false);
                socket->write("HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            });
        }
    });
}

bool Automation::configure(quint16 port, bool lan, QByteArray token)
{
    server_.close();
    token_ = std::move(token);
    if (!port)
        return true;
    return !token_.isEmpty() && server_.listen(lan ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost, port);
}
}
