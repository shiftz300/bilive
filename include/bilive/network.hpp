// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bilive/protocol.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <functional>

namespace bilive {

class Network final : public QObject {
public:
    using Completion = std::function<void(Response)>;

    explicit Network(QObject *parent = nullptr);
    // The caller's context must live in this object's Qt thread. Destroying it
    // disconnects the completion, while Network still releases the reply.
    bool setProxy(int mode, const QUrl &url = {});
    void send(const Request &, QObject *context, Completion completion);

private:
    QNetworkAccessManager manager_;
};

} // namespace bilive
