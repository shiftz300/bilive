// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/helper.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace bilive {
Helper::Helper(QString resources, QObject *parent)
    : QObject(parent), resources_(std::move(resources))
{
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { read(); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        process_.readAllStandardError();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](auto error) {
        if (error == QProcess::FailedToStart)
            fail(tr("无法启动 bilive 后台组件。"));
    });
    connect(&process_, &QProcess::finished, this, [this] {
        read();
        fail(tr("bilive 后台组件已退出。"));
    });
}

Helper::~Helper()
{
    process_.disconnect(this);
    callbacks_.clear();
    if (process_.state() != QProcess::NotRunning) {
        process_.closeWriteChannel();
        if (!process_.waitForFinished(250)) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }
}

void Helper::call(const QString &op, const QJsonObject &args, Completion completion)
{
    const auto dir = QDir(resources_).filePath(QStringLiteral("helper"));
    if (process_.state() == QProcess::NotRunning) {
        auto python = qEnvironmentVariable("BILIVE_HELPER_PYTHON");
        if (python.isEmpty()) {
#ifdef Q_OS_WIN
            python = QDir(dir).filePath(QStringLiteral("runtime/python.exe"));
#else
            python = QDir(dir).filePath(QStringLiteral("runtime/bin/python3.13"));
#endif
        }
        if (!QFileInfo::exists(python)) {
            if (completion)
                completion(Error{Error::Kind::Input, 0, tr("插件包缺少 bilive 后台运行时。")});
            return;
        }
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("PYTHONHOME"));
        env.insert(QStringLiteral("PYTHONPATH"), dir + QDir::listSeparator() +
                   QDir(dir).filePath(QStringLiteral("site")));
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
        process_.setProcessEnvironment(env);
        buffer_.clear();
        process_.start(python, {QStringLiteral("-B"), QDir(dir).filePath(QStringLiteral("bilive_helper.py"))});
    }
    const int id = ++next_;
    if (completion)
        callbacks_.insert(id, std::move(completion));
    const QJsonObject request{{QStringLiteral("id"), id}, {QStringLiteral("op"), op}, {QStringLiteral("args"), args}};
    process_.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void Helper::read()
{
    buffer_ += process_.readAllStandardOutput();
    while (true) {
        const auto newline = buffer_.indexOf('\n');
        if (newline < 0)
            break;
        const auto json = QJsonDocument::fromJson(buffer_.left(newline)).object();
        buffer_.remove(0, newline + 1);
        if (json.contains(QStringLiteral("id"))) {
            const int id = json.value(QStringLiteral("id")).toInt();
            auto callback = callbacks_.take(id);
            if (callback) {
                const auto error = json.value(QStringLiteral("error")).toString();
                if (error.isEmpty())
                    callback(json.value(QStringLiteral("value")));
                else
                    callback(Error{Error::Kind::Response, 0, error});
            }
        } else if (events_) {
            events_(json.value(QStringLiteral("event")).toString(), json.value(QStringLiteral("args")).toArray(),
                    json.value(QStringLiteral("fields")).toObject());
        }
    }
}

void Helper::fail(const QString &message)
{
    auto pending = std::exchange(callbacks_, {});
    for (const auto &callback : pending)
        callback(Error{Error::Kind::Network, 0, message});
    if (events_)
        events_(QStringLiteral("exited"), QJsonArray{message}, {});
}
}
