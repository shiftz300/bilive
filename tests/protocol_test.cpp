// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/protocol.hpp"

#include <QCoreApplication>

using namespace bilive;

namespace {
void check(bool result, const char *message)
{
    if (!result)
        qFatal("%s", message);
}

Response ok(QByteArray body, QList<QByteArray> cookies = {})
{
    return {200, {}, std::move(body), std::move(cookies)};
}

template<typename T>
T value(Result<T> result)
{
    if (const auto *error = std::get_if<Error>(&result))
        qFatal("Unexpected simulated Bilibili error: %s", qPrintable(error->message));
    return std::get<T>(std::move(result));
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const Identity identity = value(Identity::fromJson(R"({
        "ak":"demo", "as":"demo-secret", "b":"123", "v":"1.2.3",
        "start_ac":true, "stop_ac":false,
        "hw":{"User-Agent":"qr-client"}, "ha":{"User-Agent":"app-client"}
    })"));
    const Protocol protocol(identity, "test-device");
    const auto generate = protocol.generateQr(1760012345678);
    check(generate.url.host() == QStringLiteral("passport.bilibili.com") &&
              generate.url.toEncoded().contains("livehime_create_ts%3D1760012345678") &&
              generate.headers.value("user-agent") == "qr-client",
          "QR request must carry encoded redirect and web identity");
    check(value(Protocol::readQr(ok(R"({"code":0,"data":{"qrcode_key":"test-key",
          "url":"https://passport.bilibili.com/example"}})"))).key == QStringLiteral("test-key"),
          "QR payload should provide a poll key");

    check(value(Protocol::readPoll(ok(R"({"code":0,"data":{"code":86101}})"))).status ==
              LoginStatus::WaitingForScan, "poll unscanned");
    check(value(Protocol::readPoll(ok(R"({"code":0,"data":{"code":86090}})"))).status ==
              LoginStatus::WaitingForConfirmation, "poll awaiting confirmation");
    check(value(Protocol::readPoll(ok(R"({"code":0,"data":{"code":86038}})"))).status ==
              LoginStatus::Expired, "expired QR");
    const auto signedIn = value(Protocol::readPoll(ok(R"({"code":0,"data":{"code":0}})",
        {"SESSDATA=fake%2Csession; Path=/; Secure", "DedeUserID=123; Path=/",
         "bili_jct=test-csrf; Domain=.bilibili.com"})));
    check(signedIn.status == LoginStatus::SignedIn && signedIn.credentials.valid() &&
              signedIn.credentials.uid() == QStringLiteral("123"), "scan cookies should yield the correct UID");
    check(std::holds_alternative<Error>(Protocol::readPoll(ok(R"({"code":0,"data":{"code":0}})"))),
          "poll success without cookies is malformed");

    const auto &auth = signedIn.credentials;
    const auto title = protocol.updateTitle(auth, QStringLiteral("999"), QStringLiteral("猫 + 音乐"), 1760012345);
    check(title.headers.value("cookie").contains("SESSDATA=fake%2Csession"),
          "authenticated request should preserve escaped session bytes");
    check(title.headers.value("user-agent") == "app-client" &&
              title.body.contains("title=%E7%8C%AB+%2B+%E9%9F%B3%E4%B9%90"), "UTF-8 form encoding");
    const auto opening = protocol.start(auth, QStringLiteral("999"), QStringLiteral("101"), 1760012345);
    const auto ending = protocol.stop(auth, QStringLiteral("999"), 1760012345);
    check(opening.url.path().endsWith(u"/startLive") && opening.body.contains("type=2") &&
              opening.body.contains("sign=360f83dec5dedbba44df26888cfd45b4"), "start signs CSRF fields");
    check(ending.url.path().endsWith(u"/stopLive") && ending.body.contains("csrf_token=test-csrf") &&
              ending.body.contains("sign=ac13db056cc46f0303e6d9e057da20c6"), "stop includes CSRF outside signature");

    auto room = value(Protocol::readRoom(
        ok(R"({"code":0,"data":{"room_id":999,"live_status":1,"area_v2_id":"101","area_v2_name":"Music"}})"),
        ok(R"({"code":0,"data":{"title":"test","cover":{"url":"https://i0.hdslb.com/test"}}})")));
    check(room.live && room.id == QStringLiteral("999") && room.areaId == QStringLiteral("101"),
          "room combines persisted settings and current room state");
    auto duplicateAreas = value(Protocol::readAreas(ok(R"({"code":0,"data":{"area_v1_info":[
        {"name":"parent1","list":[{"id":101,"name":"duplicate"}]},
        {"name":"parent2","list":[{"id":102,"name":"duplicate"}]}
    ]}})")));
    check(duplicateAreas.size() == 2 && duplicateAreas[0].id != duplicateAreas[1].id,
          "duplicate area names must preserve IDs");

    const auto allowed = ok(R"({"code":0,"data":{
        "rtmp":{"addr":"rtmps://example.test/live","code":"fake-key"},
        "protocols":[{"protocol":"SRT","addr":"srt://example.test:1234","code":"fake-srt"}]
    }})");
    const auto selected = std::get<LiveStarted>(value(Protocol::readStart(allowed, StreamPreference::PreferSrt)));
    check(selected.target && selected.target->protocol == StreamProtocol::Srt, "select SRT when supplied");
    const auto rtmpOnly = ok(R"({"code":0,"data":{"rtmp":{"addr":"rtmp://example.test/live","code":"test"}}})");
    const auto fallback = std::get<LiveStarted>(value(Protocol::readStart(rtmpOnly, StreamPreference::PreferSrt)));
    check(fallback.target && fallback.target->protocol == StreamProtocol::Rtmp, "fall back to RTMP");
    check(!std::get<LiveStarted>(value(Protocol::readStart(rtmpOnly, StreamPreference::SrtOnly))).target,
          "SRT-only should preserve opened room state without selecting RTMP");
    const auto face = ok(R"({"code":60043,"message":"challenge","data":{"risk_extra":{"v_voucher":"mock"}}})");
    check(std::get<FaceChallenge>(value(Protocol::readStart(face, StreamPreference::Rtmp))).voucher ==
              QStringLiteral("mock"), "face verification challenge must be returned to the UI");
    auto denied = Protocol::readStart(ok(R"({"code":-403,"message":"denied"})"), StreamPreference::Rtmp);
    check(std::get<Error>(denied).code == -403, "server denial cannot become start success");
    return 0;
}
