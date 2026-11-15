// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/network.hpp"

#include <QNetworkReply>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
#include <QSslSocket>

namespace bilive {
namespace {
class SystemProxy final : public QNetworkProxyFactory {
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        return systemProxyForQuery(query);
    }
};
}


Network::Network(QObject *parent) : QObject(parent)
{
    manager_.setAutoDeleteReplies(true);
    setProxy(0);
}

bool Network::setProxy(int mode, const QUrl &url)
{
    if (mode == 0) {
        manager_.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    } else if (mode == 1) {
        manager_.setProxyFactory(new SystemProxy);
    } else if (mode == 2) {
        const auto scheme = url.scheme();
        const bool socks = scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks5h");
        if ((!socks && scheme != QStringLiteral("http")) || url.host().isEmpty() ||
            url.port(-1) <= 0 || url.port(-1) > 65535 || !url.userInfo().isEmpty())
            return false;
        manager_.setProxy(QNetworkProxy(socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                                       url.host(), quint16(url.port())));
    } else {
        return false;
    }
    return true;
}

void Network::send(const Request &request, QObject *context, Completion completion)
{
    QNetworkRequest outgoing(request.url);
    outgoing.setTransferTimeout(10000);
    outgoing.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    outgoing.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    outgoing.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    for (auto it = request.headers.begin(); it != request.headers.end(); ++it)
        outgoing.setRawHeader(it.key(), it.value());

    auto *reply = request.method == Method::Get ? manager_.get(outgoing)
                                                : manager_.post(outgoing, request.body);
    QObject::connect(reply, &QNetworkReply::finished, context,
                     [reply, completion = std::move(completion)]() {
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && response.status == 0 &&
            reply->url().scheme() == QStringLiteral("https") && !QSslSocket::supportsSsl()) {
            response.networkError = tr("无法建立 HTTPS 连接：当前 OBS 缺少可用的 Qt TLS 后端。");
        } else if (reply->error() != QNetworkReply::NoError) {
            response.networkError = tr("网络请求失败（%1）").arg(int(reply->error()));
        }
        response.body = reply->readAll();
        for (const auto &header : reply->rawHeaderPairs()) {
            if (header.first.compare("Set-Cookie", Qt::CaseInsensitive) == 0)
                response.setCookies.append(header.second);
        }
        completion(std::move(response));
    });
}

} // namespace bilive
