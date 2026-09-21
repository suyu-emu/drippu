// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QStyledItemDelegate>

class QListView;
class QPainter;
class QRect;
class QModelIndex;

// Renders one "recently played" entry per row: icon, name, play-time subtext.
class NextendoHistoryDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    static constexpr int kIconSize = 40;
    static constexpr int kCardHeight = 56;
    static constexpr int kCardRadius = 10;
    static constexpr int kCardMarginV = 3;

    explicit NextendoHistoryDelegate(QListView* view, QObject* parent = nullptr);
    ~NextendoHistoryDelegate() override;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    void initStyleOption(QStyleOptionViewItem*, const QModelIndex&) const override;

private:
    QColor CardBg() const;
    QColor FgColor() const;
    QColor DimColor() const;
    QColor AccentColor() const;

    QListView* list_view;
};
