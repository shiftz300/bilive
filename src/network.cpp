// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/network.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>

namespace bilive {

Network::Network(QObject *parent) : QObject(parent)
{
    manager_.setAutoDeleteReplies(true);
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
        if (reply->error() != QNetworkReply::NoError)
            response.networkError = reply->errorString();
        response.body = reply->readAll();
        for (const auto &header : reply->rawHeaderPairs()) {
            if (header.first.compare("Set-Cookie", Qt::CaseInsensitive) == 0)
                response.setCookies.append(header.second);
        }
        completion(std::move(response));
    });
}

} // namespace bilive
