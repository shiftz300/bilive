// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/network.hpp"
#include "bilive/accounts.hpp"
#include "bilive/cover.hpp"
#include "qrcodegen.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QCompleter>
#include <QStringListModel>
#include <QDir>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalBlocker>
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

QByteArray persistentDevice(QSettings &settings)
{
    const auto key = QStringLiteral("network/device");
    auto id = settings.value(key).toByteArray();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().toLatin1() + "user";
        settings.setValue(key, id);
    }
    return id;
}

class Dock final : public QWidget {
public:
    Dock(Identity identity, const QString &settingsPath)
        : settings_(settingsPath, QSettings::IniFormat),
          protocol_(std::move(identity), persistentDevice(settings_)), accounts_(settings_)
    {
        auto *layout = new QVBoxLayout(this);
        auto *loginRow = new QHBoxLayout;
        login_ = new QPushButton(tr("扫码登录"), this);
        refresh_ = new QPushButton(tr("刷新房间"), this);
        account_ = new QComboBox(this);
        loginRow->addWidget(account_, 1);
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
        titles_ = new QStringListModel(this);
        auto *titleCompleter = new QCompleter(titles_, title_);
        titleCompleter->setFilterMode(Qt::MatchContains);
        title_->setCompleter(titleCompleter);
        auto *titleRow = new QHBoxLayout;
        saveTitle_ = new QPushButton(tr("保存"), this);
        titleRow->addWidget(title_, 1);
        titleRow->addWidget(saveTitle_);
        form->addRow(tr("标题"), titleRow);
        area_ = new QComboBox(this);
        area_->setEditable(true);
        area_->setInsertPolicy(QComboBox::NoInsert);
        area_->completer()->setCompletionMode(QCompleter::PopupCompletion);
        area_->completer()->setFilterMode(Qt::MatchContains);
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
        auto *features = new QHBoxLayout;
        auto addFeature = [this, features](const QString &text, auto slot) {
            auto *button = new QPushButton(text, this);
            features->addWidget(button);
            connect(button, &QPushButton::clicked, this, slot);
            return button;
        };
        cover_ = addFeature(tr("封面"), [this] { editCover(); });
        delay_ = addFeature(tr("延迟"), [this] { editDelay(); });
        addFeature(tr("设置"), [this] { preferences(); });
        addFeature(tr("日志"), [this] { openLog(); });
        layout->addLayout(features);

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
        autoStart_->setChecked(settings_.value(QStringLiteral("stream/autoStart"), false).toBool());
        preference_->setCurrentIndex(qBound(0, settings_.value(QStringLiteral("stream/protocol"), 0).toInt(), 2));
        connect(autoStart_, &QCheckBox::toggled, this, [this](bool enabled) {
            settings_.setValue(QStringLiteral("stream/autoStart"), enabled);
        });
        connect(preference_, &QComboBox::currentIndexChanged, this, [this](int index) {
            settings_.setValue(QStringLiteral("stream/protocol"), index);
        });
        connect(account_, &QComboBox::activated, this, [this](int index) {
            useAccount(account_->itemData(index).toString());
        });
        faceTimer_.setSingleShot(true);
        faceTimer_.setInterval(1500);
        connect(&faceTimer_, &QTimer::timeout, this, [this] { pollFace(); });
        network_.setProxy(settings_.value(QStringLiteral("network/proxyMode"), 0).toInt(),
                          QUrl(settings_.value(QStringLiteral("network/proxy")).toString()));
        const auto savedFont = settings_.value(QStringLiteral("appearance/font")).toString();
        QFont dockFont = font();
        if (!savedFont.isEmpty() && dockFont.fromString(savedFont))
            setFont(dockFont);
        refreshAccounts();
        updateControls();
        const auto uid = settings_.value(QStringLiteral("accounts/current")).toString();
        if (!uid.isEmpty())
            QTimer::singleShot(0, this, [this, uid] { useAccount(uid); });
    }

    void outputStopped()
    {
        startedOutput_ = false;
    }

private:
    void showStatus(const QString &message)
    {
        status_->setText(message);
        if (lastStatus_ != message) {
            lastStatus_ = message;
            history_.append(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + message);
            if (history_.size() > 200)
                history_.removeFirst();
        }
    }

    void openLog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("bilive 状态记录"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *view = new QPlainTextEdit(history_.join(u'\n'), &dialog);
        view->setReadOnly(true);
        layout->addWidget(view);
        dialog.resize(560, 340);
        dialog.exec();
    }

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
        account_->setEnabled(!busy_);
        for (auto *item : QList<QWidget *>{title_, area_, saveTitle_, saveArea_, start_, announcement_, cover_, delay_})
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

    void refreshAccounts()
    {
        const QSignalBlocker blocker(account_);
        account_->clear();
        account_->addItem(tr("添加账号"), QString{});
        for (const auto &uid : accounts_.accounts())
            account_->addItem(tr("账号 %1").arg(uid), uid);
        if (credentials_.valid() && account_->findData(credentials_.uid()) < 0)
            account_->addItem(tr("账号 %1（仅本次）").arg(credentials_.uid()), credentials_.uid());
        account_->setCurrentIndex(qMax(0, account_->findData(credentials_.uid())));
    }

    void clearSession()
    {
        ++session_;
        pollTimer_.stop();
        faceTimer_.stop();
        facePending_ = false;
        faceToken_.clear();
        faceTrack_.clear();
        qr_->clear();
        qr_->hide();
        credentials_ = {};
        startedOutput_ = false;
        loginPending_ = false;
        room_.reset();
        target_.reset();
        qrKey_.clear();
        title_->clear();
        area_->clear();
        settings_.remove(QStringLiteral("accounts/current"));
        refreshAccounts();
        updateControls();
    }

    void useAccount(const QString &uid)
    {
        if (uid == credentials_.uid())
            return;
        if (uid.isEmpty()) {
            clearSession();
            loginClick();
            return;
        }
        const auto found = accounts_.load(uid);
        if (auto *error = std::get_if<Error>(&found))
            return showError(*error);
        clearSession();
        credentials_ = std::get<Credentials>(found);
        settings_.setValue(QStringLiteral("accounts/current"), uid);
        refreshAccounts();
        refreshRoom();
    }

    void loginClick()
    {
        if (credentials_.valid() || loginPending_) {
            if (credentials_.valid() && accounts_.accounts().contains(credentials_.uid())) {
                const auto removed = accounts_.remove(credentials_.uid());
                if (const auto *error = std::get_if<Error>(&removed))
                    return showError(*error);
            }
            clearSession();
            showStatus(tr("用哔哩哔哩 App 扫码登录。"));
            return;
        }
        ++session_;
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
            const auto stored = accounts_.save(credentials_);
            if (std::holds_alternative<bool>(stored))
                settings_.setValue(QStringLiteral("accounts/current"), credentials_.uid());
            else
                storeWarning_ = true;
            refreshAccounts();
            refreshRoom();
        });
    }

    void refreshRoom()
    {
        setBusy(true);
        titles_->setStringList(settings_.value(QStringLiteral("history/title/") + credentials_.uid()).toStringList());
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
                    showStatus(std::exchange(storeWarning_, false)
                                   ? tr("房间设置已读取。系统凭据库不可用，此账号只保留到退出 OBS。")
                                   : tr("房间设置已读取。"));
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
            auto recent = titles_->stringList();
            recent.removeAll(title);
            recent.prepend(title);
            while (recent.size() > 20)
                recent.removeLast();
            titles_->setStringList(recent);
            settings_.setValue(QStringLiteral("history/title/") + credentials_.uid(), recent);
            showStatus(tr("标题已保存。"));
        });
    }

    void saveArea()
    {
        const auto areaId = area_->currentText() == area_->itemText(area_->currentIndex())
                                ? area_->currentData().toString() : QString{};
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

    void editCover()
    {
        const auto png = chooseCover(this);
        if (png.isEmpty())
            return;
        setBusy(true);
        showStatus(tr("正在上传封面…"));
        network_.send(protocol_.uploadCover(credentials_, png), this, [this](const Response &reply) {
            const auto decoded = bilive::Protocol::readData(reply);
            if (const auto *error = std::get_if<Error>(&decoded)) {
                setBusy(false);
                return showError(*error);
            }
            const QUrl url(std::get<QJsonObject>(decoded).value(QStringLiteral("location")).toString());
            if (!url.isValid() || (url.scheme() != u"https" && url.scheme() != u"http") || url.host().isEmpty()) {
                setBusy(false);
                return showStatus(tr("未收到有效的封面地址。"));
            }
            network_.send(protocol_.updateCover(credentials_, url), this, [this, url](const Response &reply) {
                setBusy(false);
                const auto updated = bilive::Protocol::readData(reply);
                if (const auto *error = std::get_if<Error>(&updated))
                    return showError(*error);
                room_->cover = url;
                showStatus(tr("封面已提交。"));
            });
        });
    }

    void editDelay()
    {
        setBusy(true);
        network_.send(protocol_.timeShift(credentials_, room_->id, now()), this, [this](const Response &reply) {
            const auto decoded = bilive::Protocol::readData(reply);
            if (const auto *error = std::get_if<Error>(&decoded)) {
                setBusy(false);
                return showError(*error);
            }
            const auto data = std::get<QJsonObject>(decoded);
            const int minimum = qBound(0, data.value(QStringLiteral("min_time_shift")).toInt(10), 300);
            const int maximum = qBound(minimum, data.value(QStringLiteral("max_time_shift")).toInt(300), 600);
            bool ok = false;
            const int value = QInputDialog::getInt(this, tr("B 站推流延迟"),
                tr("秒数（0 为关闭，启用时至少 %1 秒）").arg(minimum),
                qBound(0, data.value(QStringLiteral("time_shift")).toInt(), maximum), 0, maximum, 1, &ok);
            if (!ok || (value > 0 && value < minimum)) {
                setBusy(false);
                if (ok)
                    showStatus(tr("延迟需达到 B 站规定的最小值。"));
                return;
            }
            network_.send(protocol_.setTimeShift(credentials_, room_->id, value, now()), this,
                          [this](const Response &reply) {
                setBusy(false);
                const auto result = bilive::Protocol::readData(reply);
                if (const auto *error = std::get_if<Error>(&result))
                    return showError(*error);
                showStatus(tr("推流延迟已保存。"));
            });
        });
    }

    void preferences()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("bilive 设置"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        layout->addLayout(form);
        auto *mode = new QComboBox(&dialog);
        mode->addItems({tr("直连"), tr("系统代理"), tr("自定义代理")});
        mode->setCurrentIndex(qBound(0, settings_.value(QStringLiteral("network/proxyMode"), 0).toInt(), 2));
        auto *proxy = new QLineEdit(settings_.value(QStringLiteral("network/proxy")).toString(), &dialog);
        proxy->setPlaceholderText(QStringLiteral("socks5://127.0.0.1:7890"));
        form->addRow(tr("网络"), mode);
        form->addRow(tr("代理地址"), proxy);
        QFont selectedFont = font();
        bool fontChosen = false;
        auto *fontButton = new QPushButton(tr("选择字体…"), &dialog);
        connect(fontButton, &QPushButton::clicked, &dialog, [this, &dialog, &selectedFont, &fontChosen] {
            bool accepted = false;
            const auto candidate = QFontDialog::getFont(&accepted, selectedFont, &dialog, tr("选择 bilive 字体"));
            if (accepted) {
                selectedFont = candidate;
                fontChosen = true;
            }
        });
        form->addRow(tr("停靠窗口字体"), fontButton);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (!network_.setProxy(mode->currentIndex(), QUrl(proxy->text()))) {
            showStatus(tr("请输入带端口的 HTTP 或 SOCKS5 代理地址。"));
            return;
        }
        settings_.setValue(QStringLiteral("network/proxyMode"), mode->currentIndex());
        settings_.setValue(QStringLiteral("network/proxy"), proxy->text());
        if (fontChosen) {
            settings_.setValue(QStringLiteral("appearance/font"), selectedFont.toString());
            setFont(selectedFont);
        }
    }

    void beginFace(const FaceChallenge &face)
    {
        facePending_ = true;
        faceDeadline_ = QDateTime::currentMSecsSinceEpoch() + 180000;
        faceToken_.clear();
        faceTrack_.clear();
        const auto id = session_;
        network_.send(protocol_.faceReport(credentials_, room_->id, area_->currentData().toString(),
                                          face.code, face.message, now()), this, [](const Response &) {});
        if (!face.qr.isEmpty()) {
            showQr(face.qr);
            showStatus(tr("请用手机完成人脸验证。"));
            faceTimer_.start();
            return;
        }
        if (face.voucher.isEmpty()) {
            facePending_ = false;
            return showStatus(tr("B 站未提供验证凭据。"));
        }
        showStatus(tr("正在获取人脸验证二维码…"));
        network_.send(protocol_.faceRegister(credentials_, face.voucher), this,
                      [this, face, id](const Response &reply) {
            if (id != session_ || !facePending_)
                return;
            auto result = bilive::Protocol::readFaceQr(face.voucher, reply);
            if (const auto *error = std::get_if<Error>(&result)) {
                facePending_ = false;
                return showError(*error);
            }
            auto qr = std::get<FaceQr>(std::move(result));
            faceToken_ = std::move(qr.token);
            faceTrack_ = std::move(qr.track);
            showQr(qr.url);
            showStatus(tr("请用手机完成人脸验证。"));
            faceTimer_.start();
        });
    }

    void pollFace()
    {
        if (!facePending_ || !room_)
            return;
        if (QDateTime::currentMSecsSinceEpoch() >= faceDeadline_) {
            facePending_ = false;
            qr_->hide();
            return showStatus(tr("人脸验证已超时。"));
        }
        const auto id = session_;
        network_.send(protocol_.faceStatus(credentials_, room_->id, faceToken_, faceTrack_), this,
                      [this, id](const Response &reply) {
            if (id != session_ || !facePending_)
                return;
            const auto result = bilive::Protocol::readData(reply);
            if (const auto *error = std::get_if<Error>(&result)) {
                facePending_ = false;
                qr_->hide();
                return showError(*error);
            }
            const auto data = std::get<QJsonObject>(result);
            const int state = data.value(QStringLiteral("status")).toInt();
            const auto identified = data.value(QStringLiteral("is_identified"));
            if (faceToken_.isEmpty() ? (identified.toBool() || identified.toInt() == 1) : state == 1) {
                facePending_ = false;
                qr_->hide();
                showStatus(tr("验证通过，请重新点击开播。"));
            } else if (!faceToken_.isEmpty() && state >= 2) {
                facePending_ = false;
                qr_->hide();
                showStatus(tr("人脸验证已失败或超时。"));
            } else {
                faceTimer_.start();
            }
        });
    }

    void startRoom()
    {
        if (facePending_)
            return showStatus(tr("请先完成手机上的人脸验证。"));
        const QString areaId = area_->currentText() == area_->itemText(area_->currentIndex())
                                   ? area_->currentData().toString() : QString{};
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
                beginFace(*face);
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

    QSettings settings_;
    bilive::Protocol protocol_;
    AccountStore accounts_;
    QTimer faceTimer_;
    qint64 faceDeadline_ = 0;
    bool facePending_ = false;
    bool storeWarning_ = false;
    QString faceToken_, faceTrack_, lastStatus_;
    QStringList history_;
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
    QStringListModel *titles_;
    QPushButton *cover_, *delay_;
    QComboBox *area_, *preference_, *account_;
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
    const QString wire = QString::fromUtf8(path);
    QFile file(wire);
    bfree(path);
    if (!file.open(QIODevice::ReadOnly)) {
        blog(LOG_ERROR, "[bilive] Could not read livehime.json");
        return false;
    }
    auto parsed = bilive::Identity::fromJson(file.readAll());
    if (std::holds_alternative<bilive::Error>(parsed))
        return false;

#ifdef BILIVE_QT_TLS_PATH
    const auto pluginPath = QFileInfo(wire).absoluteDir().absoluteFilePath(
        QStringLiteral(BILIVE_QT_TLS_PATH));
    QCoreApplication::addLibraryPath(QDir::cleanPath(pluginPath));
#endif

    char *config = obs_module_config_path("bilive.ini");
    if (!config)
        return false;
    const QString settings = QString::fromUtf8(config);
    bfree(config);
    QDir().mkpath(QFileInfo(settings).absolutePath());
    auto widget = std::make_unique<Dock>(std::get<bilive::Identity>(std::move(parsed)), settings);
    if (!obs_frontend_add_dock_by_id(dockId, dockId, widget.get()))
        return false;
    dock = widget.release();
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
}
