// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bilive/protocol.hpp"
#include <QJsonArray>
#include <QProcess>
#include <functional>

namespace bilive {
class Helper final : public QObject {
public:
    using Completion = std::function<void(Result<QJsonValue>)>;
    using EventHandler = std::function<void(const QString &, const QJsonArray &, const QJsonObject &)>;

    explicit Helper(QString resources, QObject *parent = nullptr);
    ~Helper() override;
    void call(const QString &op, const QJsonObject &args = {}, Completion completion = {});
    void onEvent(EventHandler handler) { events_ = std::move(handler); }

private:
    void read();
    void fail(const QString &message);
    QString resources_;
    QProcess process_;
    QMap<int, Completion> callbacks_;
    QByteArray buffer_;
    int next_ = 0;
    EventHandler events_;
};
}
