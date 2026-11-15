// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/network.hpp"
#include "qrcodegen.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <memory>

OBS_DECLARE_MODULE()

namespace {
using namespace bilive;
constexpr const char *dockId = "bilive";
constexpr const char *dockTitle = "bilive · B 站开播";

qint64 now()
{
    return QDateTime::currentSecsSinceEpoch();
}

QPixmap qrPixmap(const QUrl &url)
{
    const auto qr = qrcodegen::QrCode::encodeText(url.toEncoded().constData(),
                                                 qrcodegen::QrCode::Ecc::MEDIUM);
    const int pixel = std::max(2, 180 / (qr.getSize() + 8));
    const int width = (qr.getSize() + 8) * pixel;
    QImage image(width, width, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y))
                painter.drawRect((x + 4) * pixel, (y + 4) * pixel, pixel, pixel);
        }
    }
    painter.end();
    return QPixmap::fromImage(image);
}

class Dock final : public QWidget {
public:
    explicit Dock(Identity identity)
        : protocol_(std::move(identity),
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().toLatin1() +
                        QByteArray::number(QCoreApplication::applicationPid()) + "user")
    {
        auto *layout = new QVBoxLayout(this);
        auto *loginRow = new QHBoxLayout;
        login_ = new QPushButton(tr("扫码登录"), this);
        refresh_ = new QPushButton(tr("刷新房间"), this);
        loginRow->addWidget(login_);
        loginRow->addWidget(refresh_);
        layout->addLayout(loginRow);
        qr_ = new QLabel(this);
        qr_->setAlignment(Qt::AlignCenter);
        qr_->hide();
        layout->addWidget(qr_);

        auto *form = new QFormLayout;
        roomLabel_ = new QLabel(tr("未登录"), this);
        form->addRow(tr("直播间"), roomLabel_);
        title_ = new QLineEdit(this);
        auto *titleRow = new QHBoxLayout;
        saveTitle_ = new QPushButton(tr("保存"), this);
        titleRow->addWidget(title_, 1);
        titleRow->addWidget(saveTitle_);
        form->addRow(tr("标题"), titleRow);
        area_ = new QComboBox(this);
        auto *areaRow = new QHBoxLayout;
        saveArea_ = new QPushButton(tr("保存"), this);
        areaRow->addWidget(area_, 1);
        areaRow->addWidget(saveArea_);
        form->addRow(tr("分区"), areaRow);
        preference_ = new QComboBox(this);
        preference_->addItems({tr("RTMP"), tr("优先 SRT"), tr("仅 SRT")});
        form->addRow(tr("协议"), preference_);
        layout->addLayout(form);

        auto *liveRow = new QHBoxLayout;
        start_ = new QPushButton(tr("开播"), this);
        stop_ = new QPushButton(tr("关播"), this);
        liveRow->addWidget(start_);
        liveRow->addWidget(stop_);
        layout->addLayout(liveRow);

        auto *actions = new QHBoxLayout;
        announcement_ = new QPushButton(tr("公告"), this);
        import_ = new QPushButton(tr("导入 OBS"), this);
        key_ = new QPushButton(tr("复制串流密钥"), this);
        actions->addWidget(announcement_);
        actions->addWidget(import_);
        actions->addWidget(key_);
        layout->addLayout(actions);

        autoStart_ = new QCheckBox(tr("开播成功后启动 OBS 推流"), this);
        layout->addWidget(autoStart_);
        status_ = new QLabel(tr("用哔哩哔哩 App 扫码登录。"), this);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        layout->addStretch();

        pollTimer_.setSingleShot(true);
        pollTimer_.setInterval(1200);
        connect(&pollTimer_, &QTimer::timeout, this, [this] { pollLogin(); });
        connect(login_, &QPushButton::clicked, this, [this] { loginClick(); });
        connect(refresh_, &QPushButton::clicked, this, [this] { refreshRoom(); });
        connect(saveTitle_, &QPushButton::clicked, this, [this] { saveTitle(); });
        connect(saveArea_, &QPushButton::clicked, this, [this] { saveArea(); });
        connect(start_, &QPushButton::clicked, this, [this] { startRoom(); });
        connect(stop_, &QPushButton::clicked, this, [this] { stopRoom(); });
        connect(announcement_, &QPushButton::clicked, this, [this] { saveAnnouncement(); });
        connect(import_, &QPushButton::clicked, this, [this] {
            if (importStream())
                showStatus(tr("推流地址已导入当前 OBS 配置。"));
        });
        connect(key_, &QPushButton::clicked, this, [this] {
            if (target_) {
                QApplication::clipboard()->setText(target_->obsKey());
                showStatus(tr("串流密钥已复制。"));
            }
        });
        updateControls();
    }

    void outputStopped() { startedOutput_ = false; }

private:
    void showStatus(const QString &message) { status_->setText(message); }

    void showError(const Error &error)
    {
        showStatus(error.code ? tr("%1（%2）").arg(error.message).arg(error.code)
                              : error.message);
    }

    void setBusy(bool busy)
    {
        busy_ = busy;
        updateControls();
    }

    void updateControls()
    {
        const bool signedIn = credentials_.valid();
        const bool loaded = room_.has_value();
        login_->setText(signedIn ? tr("退出登录")
                                 : loginPending_ ? tr("取消扫码") : tr("扫码登录"));
        login_->setEnabled(!busy_);
        refresh_->setEnabled(signedIn && !busy_);
        for (auto *item : QList<QWidget *>{title_, area_, saveTitle_, saveArea_, start_, announcement_})
            item->setEnabled(loaded && !busy_);
        start_->setText(loaded && room_->live ? tr("重新获取推流地址") : tr("开播"));
        stop_->setEnabled(loaded && room_->live && !busy_);
        import_->setEnabled(target_.has_value() && !busy_);
        key_->setEnabled(target_.has_value() && !busy_);
        if (loaded)
            roomLabel_->setText(tr("%1 · %2").arg(room_->id,
                               room_->live ? tr("直播中") : tr("未开播")));
        else
            roomLabel_->setText(signedIn ? tr("房间未加载") : tr("未登录"));
    }

    void showQr(const QUrl &url)
    {
        try {
            qr_->setPixmap(qrPixmap(url));
            qr_->show();
        } catch (const std::exception &) {
            qr_->hide();
            showStatus(tr("二维码内容无法渲染。"));
        }
    }

    void loginClick()
    {
        ++session_;
        pollTimer_.stop();
        qr_->clear();
        qr_->hide();
        if (credentials_.valid() || loginPending_) {
            credentials_ = {};
            startedOutput_ = false;
            loginPending_ = false;
            room_.reset();
            target_.reset();
            qrKey_.clear();
            title_->clear();
            area_->clear();
            updateControls();
            showStatus(tr("用哔哩哔哩 App 扫码登录。"));
            return;
        }
        loginPending_ = true;
        updateControls();
        showStatus(tr("正在获取二维码…"));
        const auto session = session_;
        network_.send(protocol_.generateQr(QDateTime::currentMSecsSinceEpoch()), this,
                      [this, session](const Response &response) {
            if (session != session_)
                return;
            const auto result = bilive::Protocol::readQr(response);
            if (const auto *error = std::get_if<Error>(&result)) {
                loginPending_ = false;
                updateControls();
                return showError(*error);
            }
            const auto &code = std::get<LoginCode>(result);
            qrKey_ = code.key;
            showStatus(tr("请用哔哩哔哩 App 扫码。"));
            showQr(code.url);
            pollTimer_.start();
        });
    }

    void pollLogin()
    {
        const auto session = session_;
        network_.send(protocol_.pollQr(qrKey_), this, [this, session](const Response &response) {
            if (session != session_)
                return;
            const auto result = bilive::Protocol::readPoll(response);
            if (const auto *error = std::get_if<Error>(&result)) {
                loginPending_ = false;
                qr_->hide();
                updateControls();
                return showError(*error);
            }
            const auto &poll = std::get<LoginPoll>(result);
            if (poll.status == LoginStatus::WaitingForScan ||
                poll.status == LoginStatus::WaitingForConfirmation) {
                showStatus(poll.status == LoginStatus::WaitingForScan ? tr("等待扫码…") :
                                                                       tr("等待手机确认…"));
                pollTimer_.start();
                return;
            }
            loginPending_ = false;
            qrKey_.clear();
            qr_->hide();
            qr_->clear();
            if (poll.status == LoginStatus::Expired) {
                updateControls();
                return showStatus(tr("二维码已过期，请重新扫码。"));
            }
            credentials_ = poll.credentials;
            refreshRoom();
        });
    }

    void refreshRoom()
    {
        setBusy(true);
        showStatus(tr("正在读取房间…"));
        network_.send(protocol_.room(credentials_, now()), this, [this](Response room) {
            if (const auto decoded = bilive::Protocol::readData(room); std::holds_alternative<Error>(decoded)) {
                setBusy(false);
                return showError(std::get<Error>(decoded));
            }
            network_.send(protocol_.preLive(credentials_, now()), this,
                          [this, room = std::move(room)](const Response &preLive) {
                const auto decoded = bilive::Protocol::readRoom(room, preLive);
                if (const auto *error = std::get_if<Error>(&decoded)) {
                    setBusy(false);
                    return showError(*error);
                }
                room_ = std::get<Room>(decoded);
                if (!room_->live)
                    target_.reset();
                title_->setText(room_->title);
                area_->clear();
                if (!room_->areaId.isEmpty())
                    area_->addItem(room_->areaName, room_->areaId);

                network_.send(protocol_.areas(credentials_, now()), this, [this](const Response &response) {
                    setBusy(false);
                    const auto result = bilive::Protocol::readAreas(response);
                    if (const auto *error = std::get_if<Error>(&result))
                        return showError(*error);
                    area_->clear();
                    for (const auto &area : std::get<QList<Area>>(result))
                        area_->addItem(tr("%1 / %2").arg(area.parent, area.name), area.id);
                    area_->setCurrentIndex(area_->findData(room_->areaId));
                    showStatus(tr("房间设置已读取。"));
                });
            });
        });
    }

    void saveTitle()
    {
        const auto title = title_->text().trimmed();
        if (!room_ || title.isEmpty())
            return showStatus(tr("请输入直播标题。"));
        setBusy(true);
        network_.send(protocol_.updateTitle(credentials_, room_->id, title, now()), this,
                      [this, title](const Response &response) {
            setBusy(false);
            const auto result = bilive::Protocol::readData(response);
            if (const auto *error = std::get_if<Error>(&result))
                return showError(*error);
            room_->title = title;
            showStatus(tr("标题已保存。"));
        });
    }

    void saveArea()
    {
        const auto areaId = area_->currentData().toString();
        if (!room_ || areaId.isEmpty())
            return showStatus(tr("请选择直播分区。"));
        setBusy(true);
        network_.send(protocol_.updateArea(credentials_, room_->id, areaId, now()), this,
                      [this, areaId](const Response &response) {
            setBusy(false);
            const auto result = bilive::Protocol::readData(response);
            if (const auto *error = std::get_if<Error>(&result))
                return showError(*error);
            room_->areaId = areaId;
            showStatus(tr("分区已保存。"));
        });
    }

    void saveAnnouncement()
    {
        bool confirmed = false;
        auto value = QInputDialog::getMultiLineText(this, tr("直播间公告"), tr("公告内容"), {}, &confirmed);
        if (!confirmed)
            return;
        setBusy(true);
        network_.send(protocol_.updateAnnouncement(credentials_, value, now()), this,
                      [this](const Response &response) {
            setBusy(false);
            const auto result = bilive::Protocol::readData(response);
            if (const auto *error = std::get_if<Error>(&result))
                return showError(*error);
            showStatus(tr("公告已保存。"));
        });
    }

    void startRoom()
    {
        const QString areaId = area_->currentData().toString();
        if (!room_ || areaId.isEmpty())
            return showStatus(tr("请选择直播分区。"));
        if (obs_frontend_streaming_active())
            return showStatus(tr("OBS 正在推流，请先结束当前输出。"));
        const bool autoStart = autoStart_->isChecked();
        const auto preference = static_cast<StreamPreference>(preference_->currentIndex());
        qr_->hide();
        setBusy(true);
        showStatus(tr("正在向 B 站申请开播…"));
        network_.send(protocol_.start(credentials_, room_->id, areaId, now()), this,
                      [this, autoStart, preference](const Response &response) {
            const auto result = bilive::Protocol::readStart(response, preference);
            if (const auto *error = std::get_if<Error>(&result)) {
                setBusy(false);
                return showError(*error);
            }
            const auto &outcome = std::get<StartOutcome>(result);
            if (const auto *face = std::get_if<FaceChallenge>(&outcome)) {
                setBusy(false);
                if (!face->qr.isEmpty()) {
                    showStatus(tr("请在手机上完成人脸验证，然后再次开播。"));
                    showQr(face->qr);
                } else {
                    showStatus(tr("B 站要求新版人脸验证（%1）。当前版本尚未接入验证页面。").arg(face->code));
                }
                return;
            }
            room_->live = true;
            target_ = std::get<LiveStarted>(outcome).target;
            setBusy(false);
            if (!target_)
                return showStatus(tr("房间已开播，但 B 站没有返回所选协议的推流地址。"));
            if (!importStream())
                return;
            if (autoStart) {
                startedOutput_ = true;
                obs_frontend_streaming_start();
                showStatus(tr("房间已开播，正在启动 OBS 推流。"));
            } else {
                showStatus(tr("房间已开播，地址已导入。请在 OBS 中开始推流。"));
            }
        });
    }

    bool importStream()
    {
        if (!target_)
            return false;
        if (obs_frontend_streaming_active()) {
            showStatus(tr("OBS 正在推流，请先结束当前输出再导入地址。"));
            return false;
        }
        auto *settings = obs_data_create();
        const auto server = target_->obsServer().toUtf8();
        const auto key = target_->obsKey().toUtf8();
        obs_data_set_string(settings, "server", server.constData());
        obs_data_set_string(settings, "key", key.constData());
        obs_data_set_bool(settings, "use_auth", false);
        auto *service = obs_service_create("rtmp_custom", "bilive", settings, nullptr);
        obs_data_release(settings);
        if (!service) {
            showStatus(tr("OBS 无法创建自定义推流服务。"));
            return false;
        }
        obs_frontend_set_streaming_service(service);
        obs_service_release(service);
        obs_frontend_save_streaming_service();
        return true;
    }

    void stopRoom()
    {
        setBusy(true);
        network_.send(protocol_.stop(credentials_, room_->id, now()), this,
                      [this](const Response &response) {
            const auto result = bilive::Protocol::readData(response);
            if (const auto *error = std::get_if<Error>(&result)) {
                setBusy(false);
                return showError(*error);
            }
            if (startedOutput_ && obs_frontend_streaming_active())
                obs_frontend_streaming_stop();
            startedOutput_ = false;
            room_->live = false;
            target_.reset();
            setBusy(false);
            showStatus(tr("房间已关播。"));
        });
    }

    bilive::Protocol protocol_;
    Network network_;
    QTimer pollTimer_;
    Credentials credentials_;
    QString qrKey_;
    std::optional<Room> room_;
    std::optional<StreamTarget> target_;
    quint64 session_ = 0;
    bool busy_ = false;
    bool loginPending_ = false;
    bool startedOutput_ = false;
    QPushButton *login_, *refresh_, *saveTitle_, *saveArea_, *start_, *stop_;
    QPushButton *announcement_, *import_, *key_;
    QLineEdit *title_;
    QComboBox *area_, *preference_;
    QLabel *roomLabel_, *qr_, *status_;
    QCheckBox *autoStart_;
};

QPointer<Dock> dock;

void frontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED && dock)
        dock->outputStopped();
    if (event == OBS_FRONTEND_EVENT_EXIT && dock) {
        obs_frontend_remove_dock(dockId);
        dock = nullptr;
    }
}
} // namespace

MODULE_EXPORT const char *obs_module_name()
{
    return "bilive";
}

MODULE_EXPORT const char *obs_module_description()
{
    return "Bilibili live control dock";
}

bool obs_module_load()
{
    auto *path = obs_module_file("livehime.json");
    if (!path)
        return false;
    QFile file(QString::fromUtf8(path));
    bfree(path);
    if (!file.open(QIODevice::ReadOnly)) {
        blog(LOG_ERROR, "[bilive] Could not read livehime.json");
        return false;
    }
    auto parsed = bilive::Identity::fromJson(file.readAll());
    if (std::holds_alternative<bilive::Error>(parsed))
        return false;

    auto widget = std::make_unique<Dock>(std::get<bilive::Identity>(std::move(parsed)));
    if (!obs_frontend_add_dock_by_id(dockId, dockTitle, widget.get()))
        return false;
    dock = widget.release();
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
