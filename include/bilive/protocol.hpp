// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QUrl>

#include <optional>
#include <variant>

namespace bilive {

using Form = QMap<QString, QString>;
using Headers = QMap<QByteArray, QByteArray>;

struct Error {
    enum class Kind { Input, Network, Http, Response, Bilibili };
    Kind kind;
    int code;
    QString message;
};

template<typename T>
using Result = std::variant<T, Error>;

enum class Method { Get, Post };
struct Request {
    Method method;
    QUrl url;
    Headers headers;
    QByteArray body;
};

struct Response {
    int status = 0;
    QString networkError;
    QByteArray body;
    QList<QByteArray> setCookies;
};

struct Identity {
    QString appKey;
    QString signingKey;
    QString build;
    QString version;
    Headers webHeaders;
    Headers appHeaders;
    bool signStartCsrf = false;
    bool signStopCsrf = false;

    [[nodiscard]] static Result<Identity> fromJson(const QByteArray &);
};

struct Credentials {
    QMap<QByteArray, QByteArray> cookies;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] QString uid() const;
    [[nodiscard]] QString csrf() const;
};

struct LoginCode {
    QString key;
    QUrl url;
};

enum class LoginStatus { WaitingForScan, WaitingForConfirmation, Expired, SignedIn };
struct LoginPoll {
    LoginStatus status;
    Credentials credentials;
};

struct Room {
    QString id;
    QString title;
    QString areaId;
    QString areaName;
    QString parentAreaName;
    QUrl cover;
    bool live = false;
};

struct Area {
    QString id;
    QString name;
    QString parent;
};

enum class StreamPreference { Rtmp, PreferSrt, SrtOnly };
enum class StreamProtocol { Rtmp, Srt };
struct StreamTarget {
    StreamProtocol protocol;
    QUrl server;
    QString key;
    [[nodiscard]] QString obsServer() const;
    [[nodiscard]] QString obsKey() const;
};

struct FaceChallenge {
    int code;
    QUrl qr;
    QString voucher;
    QString message;
};

struct FaceQr {
    QUrl url;
    QString token;
    QString track;
};

struct LiveStarted {
    std::optional<StreamTarget> target;
};
using StartOutcome = std::variant<LiveStarted, FaceChallenge>;

class Protocol {
public:
    Protocol(Identity identity, QByteArray deviceId);

    [[nodiscard]] Request generateQr(qint64 nowMilliseconds) const;
    [[nodiscard]] Request pollQr(const QString &key) const;
    [[nodiscard]] Request room(const Credentials &, qint64 now) const;
    [[nodiscard]] Request preLive(const Credentials &, qint64 now) const;
    [[nodiscard]] Request areas(const Credentials &, qint64 now) const;
    [[nodiscard]] Request updateTitle(const Credentials &, const QString &roomId,
                                      const QString &title, qint64 now) const;
    [[nodiscard]] Request updateArea(const Credentials &, const QString &roomId,
                                     const QString &areaId, qint64 now) const;
    [[nodiscard]] Request updateAnnouncement(const Credentials &, const QString &text,
                                             qint64 now) const;
    [[nodiscard]] Request uploadCover(const Credentials &, const QByteArray &png) const;
    [[nodiscard]] Request updateCover(const Credentials &, const QUrl &) const;
    [[nodiscard]] Request timeShift(const Credentials &, const QString &roomId, qint64 now) const;
    [[nodiscard]] Request setTimeShift(const Credentials &, const QString &roomId, int seconds, qint64 now) const;
    [[nodiscard]] Request faceRegister(const Credentials &, const QString &voucher) const;
    [[nodiscard]] Request faceStatus(const Credentials &, const QString &roomId,
                                     const QString &token, const QString &track) const;
    [[nodiscard]] Request faceReport(const Credentials &, const QString &roomId, const QString &area,
                                     int code, const QString &message, qint64 now) const;
    [[nodiscard]] Request start(const Credentials &, const QString &roomId,
                                const QString &areaId, qint64 now) const;
    [[nodiscard]] Request stop(const Credentials &, const QString &roomId, qint64 now) const;

    [[nodiscard]] static Result<LoginCode> readQr(const Response &);
    [[nodiscard]] static Result<LoginPoll> readPoll(const Response &);
    [[nodiscard]] static Result<FaceQr> readFaceQr(const QString &voucher, const Response &);
    [[nodiscard]] static Result<QJsonObject> readData(const Response &);
    [[nodiscard]] static Result<Room> readRoom(const Response &room, const Response &preLive);
    [[nodiscard]] static Result<QList<Area>> readAreas(const Response &);
    [[nodiscard]] static Result<StartOutcome> readStart(const Response &, StreamPreference);

private:
    [[nodiscard]] Form sign(Form fields, qint64 now) const;
    [[nodiscard]] Request make(Method method, const QString &host, const QString &path,
                               const Form &query, const Form &body,
                               const Credentials *credentials = nullptr) const;
    Identity identity_;
    QByteArray deviceId_;
};

} // namespace bilive
