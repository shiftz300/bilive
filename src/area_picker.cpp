// SPDX-License-Identifier: GPL-3.0-only
#include "bilive/area_picker.hpp"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QDialog>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace bilive {

std::optional<Area> chooseArea(QWidget *parent, const QList<Area> &areas, const QString &currentId)
{
    if (areas.isEmpty())
        return std::nullopt;
    const auto tr = [](const char *text) { return QCoreApplication::translate("AreaPicker", text); };
    const auto parentName = [&](const Area &area) {
        return area.parent.isEmpty() ? tr("未分类") : area.parent;
    };
    QStringList parents;
    QHash<QString, QList<const Area *>> children;
    for (const auto &area : areas) {
        const auto name = parentName(area);
        if (!children.contains(name))
            parents.append(name);
        children[name].append(&area);
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(tr("直播分区"));
    auto *layout = new QVBoxLayout(&dialog);
    constexpr int margin = 8;
    constexpr int gap = 6;
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(gap);

    auto *selection = new QLabel(&dialog);
    selection->setTextFormat(Qt::PlainText);
    selection->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(selection);

    auto *search = new QLineEdit(&dialog);
    search->setPlaceholderText(tr("搜索子分区"));
    search->setAccessibleName(tr("搜索子分区"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);

    auto *categories = new QTabBar(&dialog);
    categories->setAccessibleName(tr("大类"));
    categories->setExpanding(false);
    categories->setUsesScrollButtons(true);
    categories->setElideMode(Qt::ElideNone);
    for (const auto &name : parents)
        categories->addTab(name);
    layout->addWidget(categories);

    auto *scroll = new QScrollArea(&dialog);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *items = new QWidget;
    auto *grid = new QGridLayout(items);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(gap);
    grid->setAlignment(Qt::AlignTop);
    scroll->setWidget(items);
    layout->addWidget(scroll);

    auto *footer = new QHBoxLayout;
    footer->setSpacing(gap);
    auto *confirm = new QPushButton(tr("确认"), &dialog);
    auto *cancel = new QPushButton(tr("取消"), &dialog);
    confirm->setDefault(true);
    cancel->setAutoDefault(false);
    footer->addWidget(confirm, 1);
    footer->addWidget(cancel, 1);
    layout->addLayout(footer);
    QObject::connect(confirm, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);

    const int screenWidth = dialog.screen()->availableGeometry().width();
    const int width = std::min(470, screenWidth - 2 * margin);
    dialog.setFixedWidth(width);
    const int scrollbarWidth = scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll);
    auto *group = new QButtonGroup(&dialog);
    const Area *chosen = nullptr;
    const auto updateSelection = [&] {
        const auto label = chosen ? tr("当前选择：%1 / %2").arg(parentName(*chosen), chosen->name)
                                  : tr("请选择子分区");
        selection->setText(selection->fontMetrics().elidedText(label, Qt::ElideRight, width - 2 * margin));
        selection->setToolTip(label);
        confirm->setEnabled(chosen != nullptr);
    };

    const auto populate = [&](const QString &retainId) {
        chosen = nullptr;
        while (auto *item = grid->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        const auto category = children.value(categories->tabText(categories->currentIndex()));
        QPushButton measure(items);
        measure.hide();
        const auto buttonText = [](QString name) { return name.replace(u'&', QStringLiteral("&&")); };
        int widest = 104;
        for (const auto *area : category) {
            measure.setText(buttonText(area->name));
            widest = std::max(widest, measure.sizeHint().width());
        }
        const int rowHeight = measure.sizeHint().height();
        const int horizontalPadding = 2 * measure.style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, &measure);
        const int usableWidth = width - 2 * margin - scrollbarWidth;
        const int columns = std::clamp((usableWidth + gap) / (widest + gap), 1, 4);
        const int cellWidth = (usableWidth - (columns - 1) * gap) / columns;
        for (int column = 0; column < 4; ++column)
            grid->setColumnStretch(column, column < columns ? 1 : 0);

        int index = 0;
        const auto keyword = search->text().trimmed();
        for (const auto *area : category) {
            if (!area->name.contains(keyword, Qt::CaseInsensitive))
                continue;
            auto *button = new QPushButton(items);
            const auto displayName = widest <= usableWidth ? area->name
                : button->fontMetrics().elidedText(area->name, Qt::ElideRight, cellWidth - horizontalPadding);
            button->setText(buttonText(displayName));
            button->setToolTip(area->name);
            button->setAccessibleName(area->name);
            button->setCheckable(true);
            button->setAutoDefault(false);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            group->addButton(button);
            grid->addWidget(button, index / columns, index % columns);
            if (area->id == retainId) {
                button->setChecked(true);
                chosen = area;
            }
            QObject::connect(button, &QPushButton::clicked, &dialog, [&, area] {
                chosen = area;
                updateSelection();
            });
            ++index;
        }

        if (index == 0)
            grid->addWidget(new QLabel(tr("没有匹配的子分区"), items), 0, 0, 1, columns);
        const int rows = std::clamp((index + columns - 1) / columns, 1, 7);
        scroll->setFixedHeight(rows * rowHeight + (rows - 1) * gap);
        scroll->verticalScrollBar()->setValue(0);
        updateSelection();
        layout->invalidate();
        dialog.setFixedHeight(layout->sizeHint().height());
        QTimer::singleShot(0, scroll, [scroll, group] {
            if (auto *checked = group->checkedButton())
                scroll->ensureWidgetVisible(checked, 0, 0);
        });
    };

    const auto current = std::ranges::find(areas, currentId, &Area::id);
    if (current != areas.end())
        categories->setCurrentIndex(parents.indexOf(parentName(*current)));
    populate(currentId);
    QObject::connect(categories, &QTabBar::currentChanged, &dialog, [&](int) { populate({}); });
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [&] {
        populate(chosen ? chosen->id : QString{});
    });
    search->setFocus();

    if (dialog.exec() == QDialog::Accepted && chosen)
        return *chosen;
    return std::nullopt;
}

} // namespace bilive
