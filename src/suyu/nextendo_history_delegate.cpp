// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "suyu/nextendo_history_delegate.h"

#include <algorithm>

#include <QApplication>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QListView>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "suyu/nextendo_account_page_p.h"
#include "suyu/nextendo_avatar_cache.h"
#include "suyu/play_time_manager.h"
#include "suyu/uisettings.h"

namespace {

QString RelativeLastPlayed(const QString& rfc3339) {
    if (rfc3339.isEmpty()) {
        return QObject::tr("never played");
    }
    const QDateTime when = QDateTime::fromString(rfc3339, Qt::ISODate);
    if (!when.isValid()) {
        return QObject::tr("never played");
    }

    const qint64 secs = when.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 60) {
        return QObject::tr("just now");
    }
    if (secs < 3600) {
        return QObject::tr("%1 min ago").arg(secs / 60);
    }
    if (secs < 86400) {
        return QObject::tr("%1 h ago").arg(secs / 3600);
    }
    return QObject::tr("%1 d ago").arg(secs / 86400);
}

} // Anonymous namespace

NextendoHistoryDelegate::NextendoHistoryDelegate(QListView* view, QObject* parent)
    : QStyledItemDelegate(parent), list_view(view) {}

NextendoHistoryDelegate::~NextendoHistoryDelegate() = default;

void NextendoHistoryDelegate::initStyleOption(QStyleOptionViewItem* option,
                                              const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    option->state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_Active |
                       QStyle::State_Sunken);
    option->showDecorationSelected = false;
}

QSize NextendoHistoryDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return QSize(0, kCardHeight);
}

void NextendoHistoryDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                            QPainter::SmoothPixmapTransform);

    const QRect card = option.rect.adjusted(4, kCardMarginV, -4, -kCardMarginV);

    QPainterPath bg_path;
    bg_path.addRoundedRect(card, kCardRadius, kCardRadius);
    painter->fillPath(bg_path, CardBg());

    const QString title_id = index.data(NextendoHistoryItem::TitleIdRole).toString();
    const std::string icon_b64 = index.data(NextendoHistoryItem::IconB64Role).toString().toStdString();
    const QRect icon_rect(card.left() + 8, card.top() + (card.height() - kIconSize) / 2, kIconSize,
                          kIconSize);

    const QPixmap icon =
        Nextendo::AvatarCache::Get(title_id.toStdString(), icon_b64, kIconSize);
    if (!icon.isNull()) {
        painter->save();
        QPainterPath clip;
        clip.addRoundedRect(icon_rect, 6, 6);
        painter->setClipPath(clip);
        painter->drawPixmap(icon_rect, icon);
        painter->restore();
    }

    const QRect text_rect(icon_rect.right() + 10, card.top(), card.right() - icon_rect.right() - 18,
                          card.height());
    const int half_h = text_rect.height() / 2;

    const QString name = index.data(NextendoHistoryItem::NameRole).toString();
    QFont name_font = option.font;
    name_font.setBold(true);
    painter->setFont(name_font);
    painter->setPen(FgColor());
    painter->drawText(QRect(text_rect.left(), text_rect.top(), text_rect.width(), half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(name_font).elidedText(name, Qt::ElideRight, text_rect.width()));

    const qulonglong seconds = index.data(NextendoHistoryItem::SecondsRole).toULongLong();
    const QString last_played = index.data(NextendoHistoryItem::LastPlayedRole).toString();
    const QString sub = tr("%1 played \xC2\xB7 %2")
                            .arg(PlayTime::ReadablePlayTime(seconds), RelativeLastPlayed(last_played));

    QFont sub_font = option.font;
    sub_font.setPointSize(std::max(sub_font.pointSize() - 1, 7));
    painter->setFont(sub_font);
    painter->setPen(DimColor());
    painter->drawText(QRect(text_rect.left(), text_rect.top() + half_h, text_rect.width(),
                            text_rect.height() - half_h),
                      Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                      QFontMetrics(sub_font).elidedText(sub, Qt::ElideRight, text_rect.width()));

    if (list_view && list_view->hasFocus() && list_view->currentIndex() == index) {
        painter->setPen(QPen(AccentColor(), 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), kCardRadius, kCardRadius);
    }

    painter->restore();
}

QColor NextendoHistoryDelegate::CardBg() const {
    return UISettings::IsDarkTheme() ? QColor(32, 32, 36) : QColor(236, 236, 240);
}

QColor NextendoHistoryDelegate::FgColor() const {
    return UISettings::IsDarkTheme() ? QColor(230, 230, 234) : QColor(22, 22, 28);
}

QColor NextendoHistoryDelegate::DimColor() const {
    return UISettings::IsDarkTheme() ? QColor(145, 145, 155) : QColor(100, 100, 112);
}

QColor NextendoHistoryDelegate::AccentColor() const {
    const QString hex = QString::fromStdString(UISettings::values.accent_color.GetValue());
    if (QColor(hex).isValid()) {
        return QColor(hex);
    }
    const QColor pa = QApplication::palette().color(QPalette::Highlight);
    return (pa.isValid() && pa != Qt::black) ? pa : QColor(100, 149, 237);
}
