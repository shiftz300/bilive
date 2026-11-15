// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bilive/protocol.hpp"
#include <QSettings>

namespace bilive {
class AccountStore {
public:
    explicit AccountStore(QSettings &settings) : settings_(settings) {}
    QStringList accounts() const;
    Result<Credentials> load(const QString &uid) const;
    Result<bool> save(const Credentials &credentials);
    Result<bool> remove(const QString &uid);
private:
    QSettings &settings_;
};
}
