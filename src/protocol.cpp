// SPDX-License-Identifier: GPL-3.0-only
// Livehime request fields and signing behavior follow StartLive 1.4.0.
#include "bilive/protocol.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkCookie>

namespace bilive {
namespace {
const QString passport = QStringLiteral("passport.bilibili.com");
const QString live = QStringLiteral("api.live.bilibili.com");

Error malformed(QString message)
{
    return {Error::Kind::Response, 0, std::move(message)};
}

QString identifier(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (!value.isDouble())
        return {};
    const auto id = value.toInteger(-1);
    return id > 0 ? QString::number(id) : QString{};
}

QByteArray encode(const Form &fields)
{
    QByteArray out;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (!out.isEmpty())
            out += '&';
        out += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
    }
    return out.replace("%20", "+");
}

Headers headers(const QJsonObject &object)
{
    Headers out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isString())
            out.insert(it.key().toLatin1().toLower(), it.value().toString().toUtf8());
    }
    return out;
}

void addCsrf(Form &form, const Credentials &credentials)
{
    form.insert(QStringLiteral("csrf"), credentials.csrf());
    form.insert(QStringLiteral("csrf_token"), credentials.csrf());
}

bool validCookie(const QByteArray &value)
{
    return !value.isEmpty() && !value.contains(';') &&
           !value.contains('\r') && !value.contains('\n') && !value.contains('\0');
}

Result<QJsonObject> envelope(const Response &reply)
{
    if (reply.status != 0 && (reply.status < 200 || reply.status >= 300))
        return Error{Error::Kind::Http, reply.status, QStringLiteral("B 站 HTTP 请求失败")};
    if (!reply.networkError.isEmpty() || reply.status == 0)
        return Error{Error::Kind::Network, 0, reply.networkError};
    const auto document = QJsonDocument::fromJson(reply.body);
    if (!document.isObject() || !document.object().value(QStringLiteral("code")).isDouble())
        return malformed(QStringLiteral("B 站响应格式异常"));
    return document.object();
}

Error apiError(const QJsonObject &object)
{
    return {Error::Kind::Bilibili, object.value(QStringLiteral("code")).toInt(),
            object.value(QStringLiteral("message")).toString(QStringLiteral("B 站拒绝请求"))};
}

std::optional<StreamTarget> asTarget(const QJsonObject &address, StreamProtocol protocol)
{
    const QUrl url(address.value(QStringLiteral("addr")).toString(), QUrl::StrictMode);
    const auto key = address.value(QStringLiteral("code")).toString();
    const QString scheme = url.scheme().toLower();
    const bool allowed = protocol == StreamProtocol::Srt
                             ? scheme == QStringLiteral("srt")
                             : scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps");
    if (!allowed || !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        key.isEmpty() || key.contains(u'\r') || key.contains(u'\n') || key.contains(QChar::Null))
        return std::nullopt;
    return StreamTarget{protocol, url, key};
}

bool httpsLink(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty();
}
} // namespace

Result<Identity> Identity::fromJson(const QByteArray &data)
{
    const auto doc = QJsonDocument::fromJson(data);
    const auto object = doc.object();
    Identity identity{
        object.value(QStringLiteral("ak")).toString(),
        object.value(QStringLiteral("as")).toString(),
        object.value(QStringLiteral("b")).toString(),
        object.value(QStringLiteral("v")).toString(),
        headers(object.value(QStringLiteral("hw")).toObject()),
        headers(object.value(QStringLiteral("ha")).toObject()),
        object.value(QStringLiteral("start_ac")).toBool(),
        object.value(QStringLiteral("stop_ac")).toBool(),
    };
    if (!doc.isObject() || identity.appKey.isEmpty() || identity.signingKey.isEmpty() ||
        identity.build.isEmpty() || identity.version.isEmpty())
        return malformed(QStringLiteral("开播接口配置不完整"));
    return identity;
}

bool Credentials::valid() const
{
    bool numeric = false;
    const qint64 mid = uid().toLongLong(&numeric);
    return numeric && mid > 0 && validCookie(cookies.value("DedeUserID")) &&
           validCookie(cookies.value("SESSDATA")) && validCookie(cookies.value("bili_jct"));
}

QString Credentials::uid() const
{
    return QString::fromLatin1(cookies.value("DedeUserID"));
}

QString Credentials::csrf() const
{
    return QString::fromLatin1(cookies.value("bili_jct"));
}

QString StreamTarget::obsServer() const
{
    return QString::fromUtf8(server.toEncoded());
}

QString StreamTarget::obsKey() const
{
    return key;
}

Protocol::Protocol(Identity identity, QByteArray deviceId)
    : identity_(std::move(identity)), deviceId_(std::move(deviceId))
{
}

Form Protocol::sign(Form fields, qint64 now) const
{
    Form signedFields{
        {QStringLiteral("access_key"), {}},
        {QStringLiteral("appkey"), identity_.appKey},
        {QStringLiteral("build"), identity_.build},
        {QStringLiteral("platform"), QStringLiteral("pc_link")},
        {QStringLiteral("ts"), QString::number(now)},
        {QStringLiteral("version"), identity_.version},
    };
    for (auto it = fields.begin(); it != fields.end(); ++it)
        signedFields.insert(it.key(), it.value());
    const auto digest = QCryptographicHash::hash(encode(signedFields) + identity_.signingKey.toUtf8(),
                                                  QCryptographicHash::Md5).toHex();
    signedFields.insert(QStringLiteral("sign"), QString::fromLatin1(digest));
    return signedFields;
}

Request Protocol::make(Method method, const QString &host, const QString &path,
                       const Form &query, const Form &body, const Credentials *credentials) const
{
    QByteArray url = "https://" + host.toLatin1() + path.toLatin1();
    if (!query.isEmpty())
        url += '?' + encode(query);

    Headers requestHeaders = host == passport ? identity_.webHeaders : identity_.appHeaders;
    requestHeaders.remove("accept-encoding");
    requestHeaders.remove("cookie");
    requestHeaders.remove("content-length");
    requestHeaders.remove("host");
    requestHeaders.insert("buvid", deviceId_);

    QByteArray cookies = "appkey=" + identity_.appKey.toLatin1() + "; buvid3=" + deviceId_;
    cookies += "; device_platform=Windows Version: 10.0 x86_64";
    if (credentials) {
        for (const auto *name : {"SESSDATA", "DedeUserID", "DedeUserID__ckMd5", "bili_jct", "sid"}) {
            const auto value = credentials->cookies.value(name);
            if (validCookie(value))
                cookies += "; " + QByteArray(name) + '=' + value;
        }
    }
    requestHeaders.insert("cookie", cookies);
    if (method == Method::Post)
        requestHeaders.insert("content-type", "application/x-www-form-urlencoded");
    return {method, QUrl::fromEncoded(url, QUrl::StrictMode), requestHeaders, encode(body)};
}

Request Protocol::generateQr(qint64 nowMilliseconds) const
{
    const QString redirect = QStringLiteral(
        "https://live.bilibili.com/p/html/live-pc-blink/mini-login-v2/"
        "?livehime_create_ts=%1&livehime_ts=%2")
        .arg(nowMilliseconds).arg(nowMilliseconds / 1000);
    return make(Method::Get, passport, QStringLiteral("/x/passport-login/web/qrcode/generate"),
                {{QStringLiteral("source"), QStringLiteral("live_pc")},
                 {QStringLiteral("go_url"), redirect},
                 {QStringLiteral("web_location"), QStringLiteral("0.0")}}, {});
}

Request Protocol::pollQr(const QString &key) const
{
    return make(Method::Get, passport, QStringLiteral("/x/passport-login/web/qrcode/poll"),
                {{QStringLiteral("qrcode_key"), key},
                 {QStringLiteral("source"), QStringLiteral("live_pc")},
                 {QStringLiteral("web_location"), QStringLiteral("0.0")}}, {});
}

Request Protocol::room(const Credentials &credentials, qint64 now) const
{
    return make(Method::Get, live, QStringLiteral("/xlive/app-blink/v1/room/GetInfo"),
                sign({{QStringLiteral("uId"), credentials.uid()}}, now), {}, &credentials);
}

Request Protocol::preLive(const Credentials &credentials, qint64 now) const
{
    return make(Method::Get, live, QStringLiteral("/xlive/app-blink/v1/preLive/PreLive"),
                sign({{QStringLiteral("title"), QStringLiteral("true")},
                      {QStringLiteral("area"), QStringLiteral("true")},
                      {QStringLiteral("cover"), QStringLiteral("true")},
                      {QStringLiteral("coverVertical"), QStringLiteral("true")},
                      {QStringLiteral("liveDirectionType"), QStringLiteral("0")},
                      {QStringLiteral("schedule"), QStringLiteral("true")},
                      {QStringLiteral("mobi_app"), QStringLiteral("pc_link")}}, now), {}, &credentials);
}

Request Protocol::areas(const Credentials &credentials, qint64 now) const
{
    return make(Method::Get, live, QStringLiteral("/xlive/app-blink/v1/preLive/GetAreaListForLive"),
                sign({}, now), {}, &credentials);
}

Request Protocol::updateTitle(const Credentials &credentials, const QString &roomId,
                              const QString &title, qint64 now) const
{
    Form form{{QStringLiteral("mobi_app"), QStringLiteral("pc_link")},
              {QStringLiteral("room_id"), roomId}, {QStringLiteral("title"), title}};
    addCsrf(form, credentials);
    return make(Method::Post, live, QStringLiteral("/xlive/app-blink/v1/preLive/UpdatePreLiveInfo"),
                sign({}, now), form, &credentials);
}

Request Protocol::updateArea(const Credentials &credentials, const QString &roomId,
                             const QString &areaId, qint64 now) const
{
    Form form{{QStringLiteral("area_id"), areaId}, {QStringLiteral("build"), identity_.build},
              {QStringLiteral("platform"), QStringLiteral("pc_link")},
              {QStringLiteral("room_id"), roomId}};
    addCsrf(form, credentials);
    return make(Method::Post, live, QStringLiteral("/xlive/app-blink/v2/room/AnchorChangeRoomArea"),
                sign({}, now), form, &credentials);
}

Request Protocol::updateAnnouncement(const Credentials &credentials, const QString &text,
                                     qint64 now) const
{
    Form form = sign({}, now);
    addCsrf(form, credentials);
    form.insert(QStringLiteral("content"), text);
    form.insert(QStringLiteral("type"), QStringLiteral("1"));
    return make(Method::Post, live, QStringLiteral("/xlive/app-blink/v1/room/AnnounceCommit"),
                {}, form, &credentials);
}

Request Protocol::start(const Credentials &credentials, const QString &roomId,
                        const QString &areaId, qint64 now) const
{
    Form form{{QStringLiteral("area_v2"), areaId}, {QStringLiteral("room_id"), roomId},
              {QStringLiteral("type"), QStringLiteral("2")}};
    if (identity_.signStartCsrf)
        addCsrf(form, credentials);
    form = sign(form, now);
    if (!identity_.signStartCsrf)
        addCsrf(form, credentials);
    return make(Method::Post, live, QStringLiteral("/room/v1/Room/startLive"), {}, form, &credentials);
}

Request Protocol::stop(const Credentials &credentials, const QString &roomId, qint64 now) const
{
    Form form{{QStringLiteral("room_id"), roomId}};
    if (identity_.signStopCsrf)
        addCsrf(form, credentials);
    form = sign(form, now);
    if (!identity_.signStopCsrf)
        addCsrf(form, credentials);
    return make(Method::Post, live, QStringLiteral("/room/v1/Room/stopLive"), {}, form, &credentials);
}

Result<QJsonObject> Protocol::readData(const Response &response)
{
    auto decoded = envelope(response);
    if (auto *error = std::get_if<Error>(&decoded))
        return *error;
    const auto &object = std::get<QJsonObject>(decoded);
    if (object.value(QStringLiteral("code")).toInt() != 0)
        return apiError(object);
    return object.value(QStringLiteral("data")).toObject();
}

Result<LoginCode> Protocol::readQr(const Response &response)
{
    auto decoded = readData(response);
    if (auto *error = std::get_if<Error>(&decoded))
        return *error;
    const auto &data = std::get<QJsonObject>(decoded);
    LoginCode qr{data.value(QStringLiteral("qrcode_key")).toString(),
                 QUrl(data.value(QStringLiteral("url")).toString(), QUrl::StrictMode)};
    if (qr.key.isEmpty() || !httpsLink(qr.url))
        return malformed(QStringLiteral("无法读取登录二维码"));
    return qr;
}

Result<LoginPoll> Protocol::readPoll(const Response &response)
{
    auto decoded = readData(response);
    if (auto *error = std::get_if<Error>(&decoded))
        return *error;
    const auto &data = std::get<QJsonObject>(decoded);
    const auto code = data.value(QStringLiteral("code")).toInt(-1);
    switch (code) {
    case 86101:
        return LoginPoll{LoginStatus::WaitingForScan, {}};
    case 86090:
        return LoginPoll{LoginStatus::WaitingForConfirmation, {}};
    case 86038:
        return LoginPoll{LoginStatus::Expired, {}};
    case 0: {
        Credentials credentials;
        for (const auto &line : response.setCookies) {
            for (const auto &cookie : QNetworkCookie::parseCookies(line))
                credentials.cookies.insert(cookie.name(), cookie.value());
        }
        if (!credentials.valid())
            return malformed(QStringLiteral("扫码成功后缺少登录凭据"));
        return LoginPoll{LoginStatus::SignedIn, credentials};
    }
    default:
        return Error{Error::Kind::Bilibili, code, data.value(QStringLiteral("message"))
                         .toString(QStringLiteral("扫码登录失败"))};
    }
}

Result<Room> Protocol::readRoom(const Response &roomResponse, const Response &preLiveResponse)
{
    auto room = readData(roomResponse);
    if (auto *error = std::get_if<Error>(&room))
        return *error;
    auto preLive = readData(preLiveResponse);
    if (auto *error = std::get_if<Error>(&preLive))
        return *error;
    const auto &data = std::get<QJsonObject>(room);
    const auto &config = std::get<QJsonObject>(preLive);
    Room result{
        identifier(data.value(QStringLiteral("room_id"))),
        config.value(QStringLiteral("title")).toString(data.value(QStringLiteral("title")).toString()),
        identifier(data.value(QStringLiteral("area_v2_id"))),
        data.value(QStringLiteral("area_v2_name")).toString(),
        data.value(QStringLiteral("parent_name")).toString(),
        QUrl(config.value(QStringLiteral("cover")).toObject().value(QStringLiteral("url")).toString()),
        data.value(QStringLiteral("live_status")).toInt() == 1,
    };
    if (result.id.isEmpty())
        return malformed(QStringLiteral("账号没有返回有效的直播间号"));
    return result;
}

Result<QList<Area>> Protocol::readAreas(const Response &response)
{
    auto decoded = readData(response);
    if (auto *error = std::get_if<Error>(&decoded))
        return *error;
    QList<Area> out;
    const auto all = std::get<QJsonObject>(decoded).value(QStringLiteral("area_v1_info")).toArray();
    for (const auto &parent : all) {
        const auto group = parent.toObject();
        const auto children = group.value(QStringLiteral("list")).toArray();
        for (const auto &child : children) {
            const auto sub = child.toObject();
            Area area{identifier(sub.value(QStringLiteral("id"))),
                      sub.value(QStringLiteral("name")).toString(),
                      group.value(QStringLiteral("name")).toString()};
            if (!area.id.isEmpty() && !area.name.isEmpty())
                out.append(area);
        }
    }
    if (out.isEmpty())
        return malformed(QStringLiteral("B 站没有返回可用分区"));
    return out;
}

Result<StartOutcome> Protocol::readStart(const Response &response, StreamPreference preference)
{
    auto decoded = envelope(response);
    if (auto *error = std::get_if<Error>(&decoded))
        return *error;
    const auto &object = std::get<QJsonObject>(decoded);
    const auto code = object.value(QStringLiteral("code")).toInt();
    const auto data = object.value(QStringLiteral("data")).toObject();
    if (code == 60024 || code == 60043) {
        const auto qr = QUrl(data.value(QStringLiteral("qr")).toString(), QUrl::StrictMode);
        return StartOutcome{FaceChallenge{
            code, httpsLink(qr) ? qr : QUrl{},
            data.value(QStringLiteral("risk_extra")).toObject().value(QStringLiteral("v_voucher")).toString(),
            object.value(QStringLiteral("message")).toString(),
        }};
    }
    if (code != 0)
        return apiError(object);
    if (preference != StreamPreference::Rtmp) {
        const auto protocols = data.value(QStringLiteral("protocols")).toArray();
        for (const auto &entry : protocols) {
            const auto protocol = entry.toObject();
            if (protocol.value(QStringLiteral("protocol")).toString().compare(u"srt", Qt::CaseInsensitive) == 0) {
                if (auto target = asTarget(protocol, StreamProtocol::Srt))
                    return StartOutcome{LiveStarted{target}};
            }
        }
    }
    if (preference == StreamPreference::SrtOnly)
        return StartOutcome{LiveStarted{}};
    return StartOutcome{LiveStarted{asTarget(data.value(QStringLiteral("rtmp")).toObject(),
                                             StreamProtocol::Rtmp)}};
}

} // namespace bilive
