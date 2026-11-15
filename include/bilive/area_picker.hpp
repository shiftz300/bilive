// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bilive/protocol.hpp"

class QWidget;

namespace bilive {

[[nodiscard]] std::optional<Area> chooseArea(QWidget *parent, const QList<Area> &areas,
                                             const QString &currentId);

} // namespace bilive
