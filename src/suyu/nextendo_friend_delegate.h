// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QMap>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QString>
#include <QStyledItemDelegate>
#include <QTimer>

class QListView;
class QPainter;
class QRect;
class QModelIndex;

// Renders one friend/request row as a hand-painted card: avatar, name, presence, action pills.
class NextendoFriendDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    static constexpr int kAvatarSize = 40;
    static constexpr int kCardHeight = 64;
    static constexpr int kCardRadius = 10;
    static constexpr int kCardMarginV = 3;

    enum class ActionHit { None, Primary, Secondary };

    explicit NextendoFriendDelegate(QListView* view, QObject* parent = nullptr);
    ~NextendoFriendDelegate() override;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    void initStyleOption(QStyleOptionViewItem*, const QModelIndex&) const override;

    ActionHit HitTestActions(const QRect& cell_rect, const QPoint& pos, bool is_request) const;

public slots:
    void AdvanceAnimations();

private:
    void PaintBackground(QPainter*, const QRect&, const QModelIndex&) const;
    void PaintAvatar(QPainter*, const QRect&, const QModelIndex&) const;
    void PaintNameAndStatus(QPainter*, const QRect&, const QStyleOptionViewItem&,
                            const QModelIndex&) const;
    void PaintActions(QPainter*, const QRect& card_rect, bool is_request,
                      const QString& pill_label, qreal hover) const;
    QRect ActionsRect(const QRect& card_rect) const;

    QColor CardBg() const;
    QColor FgColor() const;
    QColor DimColor() const;
    QColor AccentColor() const;
    QColor PresenceColor(int status) const;

    QListView* list_view;
    QTimer* anim_timer;

    mutable QMap<QPersistentModelIndex, qreal> hover_prog;
    mutable QPoint last_cursor_pos;
    mutable QPersistentModelIndex last_controller_index;
    mutable bool mouse_is_driving = true;
};
