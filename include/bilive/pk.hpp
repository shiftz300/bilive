// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bilive/helper.hpp"
#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPushButton;
class QSettings;
class QTimer;
struct obs_source;

namespace bilive::capture {
void registerOutputs();
bool begin();
void end();
}
namespace bilive {
void registerPeerSource();

class PkDialog final : public QDialog {
public:
    PkDialog(Helper &helper, QSettings &settings, QWidget *parent);
    ~PkDialog() override;
    void setContext(QJsonObject context);
    void receive(const QString &type, const QJsonArray &args, const QJsonObject &fields);
    void retire();

private:
    void command(const QString &op, const QJsonObject &args = {});
    void start(bool listen);
    void search();
    void settings();
    void encoder();
    void output();
    void peerSource();
    void startPreview(const QJsonObject &args);
    void showStatus(const QString &text);
    Helper &helper_;
    QSettings &settings_;
    QJsonObject context_;
    QTimer *clock_;
    QComboBox *mode_, *candidate_;
    QCheckBox *continuous_;
    QLineEdit *search_;
    QLabel *status_, *round_, *local_, *peer_;
    QPushButton *start_, *listen_;
    QPointer<QMessageBox> invitation_;
    obs_source *source_ = nullptr;
    bool session_ = false;
};
}
