// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/cover.hpp"
#include <QBuffer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QVBoxLayout>

namespace bilive {
QByteArray chooseCover(QWidget *parent)
{
    const auto path = QFileDialog::getOpenFileName(parent, QObject::tr("打开封面图片"), {},
                                                   QObject::tr("图片 (*.jpg *.jpeg *.png *.webp *.bmp)"));
    if (path.isEmpty())
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull() || image.width() < 16 || image.height() < 9)
        return {};

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("裁剪直播封面"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    layout->addWidget(preview);
    auto *x = new QSlider(Qt::Horizontal, &dialog);
    auto *y = new QSlider(Qt::Horizontal, &dialog);
    auto *zoom = new QSlider(Qt::Horizontal, &dialog);
    for (auto *slider : {x, y, zoom})
        slider->setRange(0, 100);
    x->setValue(50);
    y->setValue(50);
    for (auto pair : {qMakePair(QObject::tr("水平位置"), x), qMakePair(QObject::tr("垂直位置"), y),
                      qMakePair(QObject::tr("缩放"), zoom)}) {
        layout->addWidget(new QLabel(pair.first, &dialog));
        layout->addWidget(pair.second);
    }
    const int maximum = std::min(image.width() / 16, image.height() / 9);
    auto crop = [&] {
        const int unit = std::max(1, qRound(maximum / (1 + 2 * zoom->value() / 100.0)));
        const int w = 16 * unit;
        const int h = 9 * unit;
        return image.copy((image.width() - w) * x->value() / 100, (image.height() - h) * y->value() / 100, w, h);
    };
    auto update = [&] {
        preview->setPixmap(QPixmap::fromImage(crop().scaled(352, 198, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
    };
    for (auto *slider : {x, y, zoom})
        QObject::connect(slider, &QSlider::valueChanged, &dialog, update);
    update();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    crop().scaled(704, 396, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(&output, "PNG");
    return bytes;
}
}
