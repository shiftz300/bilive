// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QHostAddress>
#include <QTcpServer>
#include <functional>

namespace bilive {
class Automation final : public QObject {
public:
    using Action = std::function<bool(bool start)>;
    explicit Automation(Action);
    bool configure(quint16 port, bool lan, QByteArray token);
    QString errorString() const { return server_.errorString(); }

private:
    QTcpServer server_;
    Action action_;
    QByteArray token_;
};
}
