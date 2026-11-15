// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/pk.hpp"

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace bilive {
namespace {
constexpr const char *peerId = "bilive_pk_peer";

QString label(const QString &key)
{
    static const QMap<QString, QString> labels{
        {QStringLiteral("accept_pk"), QStringLiteral("接受 PK 邀请")},
        {QStringLiteral("accept_unfollowed_pk"), QStringLiteral("接受未关注用户的 PK")},
        {QStringLiteral("classic_pk"), QStringLiteral("经典 PK")},
        {QStringLiteral("classic_opponent_voice"), QStringLiteral("经典 PK 对方语音")},
        {QStringLiteral("accept_connection"), QStringLiteral("接受连线")},
        {QStringLiteral("mutual_only"), QStringLiteral("仅互关")},
        {QStringLiteral("allow_apply_join"), QStringLiteral("允许申请连线")},
        {QStringLiteral("allow_auto_join"), QStringLiteral("允许自动连线")},
        {QStringLiteral("allow_other_invite"), QStringLiteral("允许其他主播邀请")},
    };
    return labels.value(key, key);
}

void buttons(QDialog &dialog, QVBoxLayout *layout)
{
    auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
}
} // namespace

void registerPeerSource()
{
    obs_source_info info{};
    info.id = peerId;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = [](void *) { return "bilive PK 对端"; };
    info.create = [](obs_data_t *, obs_source_t *source) -> void * { return source; };
    info.destroy = [](void *) {};
    obs_register_source(&info);
}

PkDialog::PkDialog(Helper &helper, QSettings &settings, QWidget *parent)
    : QDialog(parent), helper_(helper), settings_(settings)
{
    setWindowTitle(tr("bilive PK / 连线"));
    auto *layout = new QVBoxLayout(this);
    auto *mode = new QHBoxLayout;
    mode_ = new QComboBox(this);
    mode_->addItem(tr("经典 PK"), QStringLiteral("traditional"));
    mode_->addItem(tr("视频 PK"), QStringLiteral("video"));
    mode_->addItem(tr("视频连线"), QStringLiteral("video_link"));
    mode->addWidget(mode_);
    continuous_ = new QCheckBox(tr("连战"), this);
    mode->addWidget(continuous_);
    layout->addLayout(mode);

    auto *lookup = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("输入对方 UID 或房间号"));
    candidate_ = new QComboBox(this);
    candidate_->addItem(tr("随机匹配"), QString{});
    auto *find = new QPushButton(tr("搜索"), this);
    lookup->addWidget(search_);
    lookup->addWidget(find);
    lookup->addWidget(candidate_);
    layout->addLayout(lookup);
    connect(find, &QPushButton::clicked, this, [this] { search(); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { search(); });

    auto *commands = new QHBoxLayout;
    auto add = [this](QHBoxLayout *row, const QString &name, auto callback) {
        auto *button = new QPushButton(name, this);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, callback);
        return button;
    };
    start_ = add(commands, tr("开始"), [this] { start(false); });
    listen_ = add(commands, tr("监听邀请"), [this] { start(true); });
    add(commands, tr("结束本轮"), [this] { command(QStringLiteral("stop_round")); });
    add(commands, tr("断开连线"), [this] { command(QStringLiteral("disconnect")); });
    add(commands, tr("转为 PK"), [this] { command(QStringLiteral("start_connection_pk")); });
    layout->addLayout(commands);

    auto *options = new QHBoxLayout;
    add(options, tr("PK 设置"), [this] { this->settings(); });
    add(options, tr("视频编码"), [this] { encoder(); });
    add(options, tr("添加对端源"), [this] { peerSource(); });
    add(options, tr("画面截取"), [this] {
        QDialog crop(this);
        crop.setWindowTitle(tr("发送画面位置"));
        auto *rows = new QVBoxLayout(&crop);
        auto *x = new QSlider(Qt::Horizontal, &crop);
        auto *y = new QSlider(Qt::Horizontal, &crop);
        rows->addWidget(new QLabel(tr("水平"), &crop));
        rows->addWidget(x);
        rows->addWidget(new QLabel(tr("垂直"), &crop));
        rows->addWidget(y);
        for (auto *slider : {x, y})
            slider->setRange(0, 100);
        x->setValue(settings_.value(QStringLiteral("pk/cropX"), 50).toInt());
        y->setValue(settings_.value(QStringLiteral("pk/cropY"), 50).toInt());
        buttons(crop, rows);
        if (crop.exec() == QDialog::Accepted) {
            settings_.setValue(QStringLiteral("pk/cropX"), x->value());
            settings_.setValue(QStringLiteral("pk/cropY"), y->value());
            command(QStringLiteral("crop"), {{QStringLiteral("x"), x->value() / 100.0},
                                             {QStringLiteral("y"), y->value() / 100.0}});
        }
    });
    layout->addLayout(options);
    auto *preview = new QHBoxLayout;
    local_ = new QLabel(tr("本地预览"), this);
    peer_ = new QLabel(tr("对端预览"), this);
    for (auto *view : {local_, peer_}) {
        view->setFixedSize(280, 224);
        view->setAlignment(Qt::AlignCenter);
        preview->addWidget(view);
    }
    layout->addLayout(preview);

    auto *mute = new QHBoxLayout;
    for (const bool local : {true, false}) {
        for (const bool audio : {true, false}) {
            auto *control = new QCheckBox(local ? (audio ? tr("本地静音") : tr("关闭本地画面"))
                                                : (audio ? tr("对端静音") : tr("关闭对端画面")), this);
            mute->addWidget(control);
            connect(control, &QCheckBox::clicked, this, [this, local, audio](bool checked) {
                command(local ? QStringLiteral("set_publisher_muted") : QStringLiteral("set_remote_muted"),
                        {{QStringLiteral("values"), QJsonArray{
                             audio ? QStringLiteral("audio") : QStringLiteral("video"), checked}}});
            });
        }
    }
    layout->addLayout(mute);
    round_ = new QLabel(this);
    round_->setWordWrap(true);
    round_->setTextFormat(Qt::PlainText);
    layout->addWidget(round_);
    status_ = new QLabel(tr("开播并开始 OBS 推流后可使用 PK。"), this);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    connect(continuous_, &QCheckBox::clicked, this, [this](bool checked) {
        if (session_)
            command(QStringLiteral("set_continuous"), {{QStringLiteral("values"), QJsonArray{checked}}});
    });
    clock_ = new QTimer(this);
    clock_->setInterval(600);
    connect(clock_, &QTimer::timeout, this, [this] { output(); });
}

PkDialog::~PkDialog()
{
    retire();
    if (source_)
        obs_source_release(source_);
}

void PkDialog::retire()
{
    clock_->stop();
    if (session_)
        helper_.call(QStringLiteral("close"));
    session_ = false;
    if (source_)
        obs_source_output_video(source_, nullptr);
    capture::end();
    start_->setEnabled(true);
    listen_->setEnabled(true);
}

void PkDialog::setContext(QJsonObject context)
{
    if (context != context_) {
        retire();
        context_ = std::move(context);
        candidate_->clear();
        candidate_->addItem(tr("随机匹配"), QString{});
    }
    if (!session_)
        helper_.call(QStringLiteral("account"), context_, [this](auto result) {
            if (const auto *error = std::get_if<Error>(&result))
                showStatus(error->message);
        });
}

void PkDialog::showStatus(const QString &text) { status_->setText(text); }

void PkDialog::command(const QString &op, const QJsonObject &args)
{
    helper_.call(op, args, [this](auto result) {
        if (const auto *error = std::get_if<Error>(&result))
            showStatus(error->message);
    });
}

void PkDialog::output()
{
    if (!session_)
        return;
    auto *output = obs_frontend_get_streaming_output();
    const bool reconnecting = output && obs_output_reconnecting(output);
    obs_output_release(output);
    helper_.call(QStringLiteral("output"), {{QStringLiteral("active"), obs_frontend_streaming_active()},
                                            {QStringLiteral("reconnecting"), reconnecting}});
}

void PkDialog::start(bool listen)
{
    if (session_)
        return;
    if (!obs_frontend_streaming_active()) {
        showStatus(tr("请先开始 OBS 推流。"));
        return;
    }
    if (!capture::begin()) {
        showStatus(tr("无法开启 OBS PK 采集。"));
        return;
    }
    session_ = true;
    start_->setEnabled(false);
    listen_->setEnabled(false);
    output();
    const QJsonObject args{{QStringLiteral("mode"), mode_->currentData().toString()},
                           {QStringLiteral("peer"), candidate_->currentData().toString()},
                           {QStringLiteral("continuous"), continuous_->isChecked()},
                           {QStringLiteral("listen"), listen}};
    startPreview(args);
}

void PkDialog::startPreview(const QJsonObject &args)
{
    auto mainArgs = settings_.value(QStringLiteral("pk/encoder")).toByteArray();
    const auto encoder = QJsonDocument::fromJson(mainArgs).object();
    if (!encoder.isEmpty())
        command(QStringLiteral("encoder"), encoder);
    command(QStringLiteral("crop"), {{QStringLiteral("x"), settings_.value(QStringLiteral("pk/cropX"), 50).toInt() / 100.0},
                                     {QStringLiteral("y"), settings_.value(QStringLiteral("pk/cropY"), 50).toInt() / 100.0}});
    helper_.call(QStringLiteral("pk_start"), args, [this](auto result) {
        if (const auto *error = std::get_if<Error>(&result)) {
            retire();
            showStatus(error->message);
        } else {
            showStatus(tr("正在建立 PK 会话…"));
            clock_->start();
        }
    });
}

void PkDialog::search()
{
    const auto query = search_->text().trimmed();
    if (query.isEmpty())
        return;
    helper_.call(QStringLiteral("search"), {{QStringLiteral("text"), query}}, [this](auto result) {
        if (const auto *error = std::get_if<Error>(&result))
            return showStatus(error->message);
        const auto users = std::get<QJsonValue>(result).toObject().value(QStringLiteral("users")).toArray();
        candidate_->clear();
        candidate_->addItem(tr("随机匹配"), QString{});
        for (const auto &user : users) {
            const auto item = user.toObject();
            candidate_->addItem(item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("uid")).toString());
        }
        if (candidate_->count() > 1)
            candidate_->setCurrentIndex(1);
        showStatus(tr("找到 %1 位候选主播。").arg(users.size()));
    });
}

void PkDialog::settings()
{
    helper_.call(QStringLiteral("pk_settings"), {}, [this](auto result) {
        if (const auto *error = std::get_if<Error>(&result))
            return showStatus(error->message);
        const auto object = std::get<QJsonValue>(result).toObject();
        const auto preferences = object.value(QStringLiteral("preferences")).toObject();
        QDialog dialog(this);
        dialog.setWindowTitle(tr("PK / 连线设置"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        layout->addLayout(form);
        auto *duration = new QComboBox(&dialog);
        auto *punish = new QComboBox(&dialog);
        for (const auto &time : object.value(QStringLiteral("duration_options")).toArray())
            duration->addItem(QString::number(time.toInt()), time.toInt());
        for (const auto &time : object.value(QStringLiteral("punish_time_options")).toArray())
            punish->addItem(QString::number(time.toInt()), time.toInt());
        duration->setCurrentIndex(duration->findData(preferences.value(QStringLiteral("duration")).toInt()));
        punish->setCurrentIndex(punish->findData(object.value(QStringLiteral("punish_time")).toInt()));
        form->addRow(tr("PK 时长"), duration);
        form->addRow(tr("惩罚时长"), punish);
        QMap<QString, QCheckBox *> switches;
        for (auto it = preferences.begin(); it != preferences.end(); ++it) {
            if (it.value().isBool()) {
                auto *check = new QCheckBox(&dialog);
                check->setChecked(it.value().toBool());
                switches.insert(it.key(), check);
                form->addRow(label(it.key()), check);
            }
        }
        buttons(dialog, layout);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QJsonObject state{{QStringLiteral("duration"), duration->currentData().toInt()}};
        for (auto it = switches.begin(); it != switches.end(); ++it)
            state[it.key()] = it.value()->isChecked();
        helper_.call(QStringLiteral("pk_settings_save"),
                     {{QStringLiteral("preferences"), state}, {QStringLiteral("punish_time"), punish->currentData().toInt()}},
                     [this](auto saved) {
            if (const auto *error = std::get_if<Error>(&saved))
                showStatus(error->message);
            else
                showStatus(tr("PK 设置已保存。"));
        });
    });
}

void PkDialog::encoder()
{
    const auto path = QStringLiteral("pk/encoder");
    QDialog dialog(this);
    dialog.setWindowTitle(tr("视频编码"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *name = new QComboBox(&dialog);
    name->addItems({QStringLiteral("auto"), QStringLiteral("libx264"), QStringLiteral("libsvtav1")});
#ifdef Q_OS_MACOS
    name->addItem(QStringLiteral("h264_videotoolbox"));
#elif defined(Q_OS_WIN)
    name->addItems({QStringLiteral("h264_nvenc"), QStringLiteral("av1_nvenc"), QStringLiteral("h264_amf"),
                    QStringLiteral("av1_amf"), QStringLiteral("h264_qsv"), QStringLiteral("av1_qsv")});
#else
    name->addItems({QStringLiteral("h264_nvenc"), QStringLiteral("av1_nvenc"),
                    QStringLiteral("h264_vaapi"), QStringLiteral("h264_qsv"), QStringLiteral("av1_qsv")});
#endif
    auto *settings = new QLineEdit(&dialog);
    auto *aq = new QCheckBox(&dialog);
    auto *scale = new QComboBox(&dialog);
    scale->addItems({QStringLiteral("lanczos"), QStringLiteral("bicubic"), QStringLiteral("bilinear")});
    const auto saved = QJsonDocument::fromJson(settings_.value(path).toByteArray()).object();
    name->setCurrentText(saved.value(QStringLiteral("encoder")).toString(QStringLiteral("libx264")));
    settings->setText(saved.value(QStringLiteral("preset")).toString());
    aq->setChecked(saved.value(QStringLiteral("spatial_aq")).toBool());
    scale->setCurrentText(saved.value(QStringLiteral("scale_mode")).toString(QStringLiteral("lanczos")));
    form->addRow(tr("编码器"), name);
    form->addRow(tr("预设（空白使用默认）"), settings);
    form->addRow(tr("空间自适应量化"), aq);
    form->addRow(tr("缩放算法"), scale);
    auto *scan = new QPushButton(tr("检测可用编码器"), &dialog);
    layout->addWidget(scan);
    connect(scan, &QPushButton::clicked, &dialog, [this, scan, dialogGuard = QPointer<QDialog>(&dialog)] {
        scan->setEnabled(false);
        scan->setText(tr("正在检测…"));
        helper_.call(QStringLiteral("encoder_scan"), {}, [this, scan, dialogGuard](auto result) {
            if (!dialogGuard)
                return;
            scan->setEnabled(true);
            scan->setText(tr("检测可用编码器"));
            if (const auto *error = std::get_if<Error>(&result))
                return showStatus(error->message);
            const auto entries = std::get<QJsonValue>(result).toArray().at(0).toArray();
            QStringList available;
            for (const auto &entry : entries) {
                const auto name = entry.toObject().value(QStringLiteral("name")).toString();
                if (!name.isEmpty())
                    available.append(name);
            }
            QMessageBox::information(dialogGuard, tr("编码器检测"),
                                     available.isEmpty() ? tr("没有可用的编码器。") : available.join(QStringLiteral("\n")));
        });
    });
    buttons(dialog, layout);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject value{{QStringLiteral("encoder"), name->currentText()},
                      {QStringLiteral("spatial_aq"), aq->isChecked()},
                      {QStringLiteral("scale_mode"), scale->currentText()}};
    if (!settings->text().trimmed().isEmpty())
        value[QStringLiteral("preset")] = settings->text().trimmed();
    helper_.call(QStringLiteral("encoder"), value, [this, value, path](auto result) {
        if (const auto *error = std::get_if<Error>(&result))
            return showStatus(error->message);
        settings_.setValue(path, QJsonDocument(value).toJson(QJsonDocument::Compact));
        showStatus(tr("编码偏好已保存。"));
    });
}

void PkDialog::peerSource()
{
    auto *sceneSource = obs_frontend_get_current_scene();
    auto *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
    if (scene) {
        if (!source_)
            source_ = obs_source_create(peerId, obs_source_get_display_name(peerId), nullptr, nullptr);
        if (source_ && !obs_scene_find_source(scene, obs_source_get_name(source_)))
            obs_scene_add(scene, source_);
    }
    obs_source_release(sceneSource);
}

void PkDialog::receive(const QString &type, const QJsonArray &args, const QJsonObject &fields)
{
    if (type == QStringLiteral("set_status") || type == QStringLiteral("set_error")) {
        showStatus(args.at(0).toString());
    } else if (type == QStringLiteral("exited") || type == QStringLiteral("session_closed")) {
        session_ = false;
        retire();
        if (type == QStringLiteral("exited"))
            showStatus(args.at(0).toString());
    } else if (type == QStringLiteral("set_round_info")) {
        const auto round = args.at(0);
        if (round.isNull() || round.isUndefined()) {
            round_->clear();
        } else {
            QStringList lines;
            const auto object = round.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.value().isString() && !it.value().toString().isEmpty())
                    lines.append(it.value().toString());
                else if (it.value().isDouble())
                    lines.append(QString::number(it.value().toDouble()));
            }
            round_->setText(lines.join(QStringLiteral(" · ")));
        }
    } else if (type == QStringLiteral("invitation")) {
        if (invitation_)
            invitation_->close();
        auto *box = new QMessageBox(QMessageBox::Question, tr("PK / 连线邀请"), tr("接受主播 %1 的邀请？").arg(args.at(0).toString()),
                                    QMessageBox::Yes | QMessageBox::No, this);
        box->setTextFormat(Qt::PlainText);
        box->setAttribute(Qt::WA_DeleteOnClose);
        invitation_ = box;
        connect(box, &QMessageBox::finished, this, [this](int answer) {
            command(QStringLiteral("invitation"), {{QStringLiteral("accept"), answer == QMessageBox::Yes}});
        });
        box->open();
    } else if (type == QStringLiteral("set_invitation_remaining") && invitation_) {
        invitation_->setInformativeText(tr("还剩 %1 秒").arg(args.at(0).toInt()));
    } else if (type == QStringLiteral("dismiss_invitation") && invitation_) {
        invitation_->blockSignals(true);
        invitation_->close();
    } else if (type == QStringLiteral("set_continuous_status")) {
        const QSignalBlocker block(continuous_);
        const auto value = args.isEmpty() ? fields.value(QStringLiteral("enabled")) : args.at(0);
        if (value.isBool())
            continuous_->setChecked(value.toBool());
    } else if (type == QStringLiteral("local_video") || type == QStringLiteral("peer_video")) {
        const bool remote = type == QStringLiteral("peer_video");
        const auto encoded = QByteArray::fromBase64(args.at(0).toString().toLatin1());
        auto image = QImage::fromData(encoded).convertToFormat(QImage::Format_ARGB32);
        if (image.isNull())
            return;
        auto *widget = remote ? peer_ : local_;
        if (isVisible())
            widget->setPixmap(QPixmap::fromImage(image.scaled(widget->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        if (remote && source_) {
            obs_source_frame frame{};
            frame.data[0] = image.bits();
            frame.linesize[0] = uint32_t(image.bytesPerLine());
            frame.width = uint32_t(image.width());
            frame.height = uint32_t(image.height());
            frame.timestamp = os_gettime_ns();
            frame.format = VIDEO_FORMAT_BGRA;
            frame.full_range = true;
            obs_source_output_video(source_, &frame);
        }
    } else if (type == QStringLiteral("peer_audio") && source_) {
        const auto pcm = QByteArray::fromBase64(args.at(0).toString().toLatin1());
        const int samples = args.at(1).toInt();
        const int frames = args.at(2).toInt();
        if (frames > 0 && frames <= 4800 && pcm.size() == frames * 4 && samples >= 8000 && samples <= 192000) {
            obs_source_audio audio{};
            audio.data[0] = reinterpret_cast<const uint8_t *>(pcm.constData());
            audio.frames = uint32_t(frames);
            audio.speakers = SPEAKERS_STEREO;
            audio.format = AUDIO_FORMAT_16BIT;
            audio.samples_per_sec = uint32_t(samples);
            audio.timestamp = os_gettime_ns();
            obs_source_output_audio(source_, &audio);
        }
    }
}
} // namespace bilive
