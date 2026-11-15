// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QByteArray>
class QWidget;

namespace bilive {
// Select and crop a local image into a 704 × 396 PNG.
QByteArray chooseCover(QWidget *parent);
}
