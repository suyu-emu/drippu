// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include "suyu/mode_selector.h"

namespace {
constexpr auto kSettingsKey = "General/AppMode";
constexpr auto kRememberKey = "General/RememberMode";

// Use inline functions to avoid file-scope QString construction before QApplication
inline QString GamerDesc() {
    return QStringLiteral("A focused game library with controller support, Steam integration, "
                          "and optimized rendering.");
}
inline QString ProgrammerDesc() {
    return QStringLiteral("Developer tools, live reload, compilation workflows, and project "
                          "management.");
}
inline QString HackerDesc() {
    return QStringLiteral("Advanced configuration, memory inspection, plugins, and low-level "
                          "debugging.");
}

QPixmap LoadBrandLogo(int target_size) {
    QPixmap logo(QStringLiteral(":/img/suyu.svg"));
    if (!logo.isNull()) {
        return logo.scaled(target_size, target_size, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
    }
    return {};
}
} // anonymous namespace

ModeSelector::ModeSelector(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("drippu | Setup Profile"));
    setMinimumSize(900, 600);
    resize(1040, 660);
    setStyleSheet(QStringLiteral(
        "ModeSelector {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #170d22, "
        "              stop:0.55 #0b0711, stop:1 #060309);"
        "}"
        "QFrame#ModeSelectorFrame {"
        "  background-color: rgba(17, 11, 25, 232);"
        "  border: 1px solid rgba(255,255,255,0.13);"
        "  border-radius: 24px;"
        "}"
        "QLabel#WelcomeLabel {"
        "  color: #a78bbd;"
        "  font-size: 10pt;"
        "  font-weight: 600;"
        "}"
        "QLabel#TitleLabel {"
        "  color: #ffffff;"
        "  font-size: 21pt;"
        "  font-weight: 700;"
        "}"
        "QPushButton#ModeCard {"
        "  color: white;"
        "  background-color: rgba(255,255,255,0.065);"
        "  border: 1px solid rgba(255,255,255,0.14);"
        "  border-radius: 20px;"
        "  padding: 0;"
        "  outline: none;"
        "}"
        "QPushButton#ModeCard[recommended=\"true\"] {"
        "  border-color: rgba(168,85,247,0.82);"
        "  background-color: rgba(168,85,247,0.12);"
        "}"
        "QPushButton#ModeCard:hover {"
        "  border-color: #b86cff;"
        "  background-color: rgba(168,85,247,0.16);"
        "}"
        "QPushButton#ModeCard:focus {"
        "  border: 2px solid #c084fc;"
        "}"
        "QPushButton#ModeCard:pressed {"
        "  background-color: rgba(168,85,247,0.09);"
        "}"
        "QLabel#CardBadge {"
        "  color: #e9d5ff;"
        "  background-color: rgba(168,85,247,0.28);"
        "  border: 1px solid rgba(216,180,254,0.28);"
        "  border-radius: 8px;"
        "  padding: 3px 7px;"
        "  font-size: 8pt;"
        "  font-weight: 700;"
        "}"
        "QLabel#CardTitle {"
        "  color: #ffffff;"
        "  font-size: 19pt;"
        "  font-weight: 700;"
        "}"
        "QLabel#CardSubtitle {"
        "  color: #d8b4fe;"
        "  font-size: 10pt;"
        "  font-weight: 600;"
        "}"
        "QLabel#CardDescription {"
        "  color: #c9c4d1;"
        "  font-size: 10pt;"
        "}"
        "QLabel#CardAction {"
        "  color: #e9d5ff;"
        "  font-size: 10pt;"
        "  font-weight: 700;"
        "}"
        "QFrame#ProfileHint {"
        "  background-color: rgba(255,255,255,0.05);"
        "  border: 1px solid rgba(255,255,255,0.08);"
        "  border-radius: 12px;"
        "}"
        "QLabel#ProfileHintText {"
        "  color: #aaa4b3;"
        "  font-size: 9pt;"
        "}"
    ));

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 18, 18, 18);

    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("ModeSelectorFrame"));
    auto* frame_layout = new QVBoxLayout(frame);
    frame_layout->setContentsMargins(28, 24, 28, 22);
    frame_layout->setSpacing(18);

    auto* brand_block = new QWidget(frame);
    auto* brand_layout = new QVBoxLayout(brand_block);
    brand_layout->setContentsMargins(0, 0, 0, 0);
    brand_layout->setSpacing(5);

    auto* brand_identity = new QWidget(brand_block);
    auto* brand_identity_layout = new QHBoxLayout(brand_identity);
    brand_identity_layout->setContentsMargins(0, 0, 0, 0);
    brand_identity_layout->setSpacing(10);
    brand_identity_layout->setAlignment(Qt::AlignCenter);

    auto* brand_logo = new QLabel(brand_identity);
    brand_logo->setAlignment(Qt::AlignCenter);
    const QPixmap logo_px = LoadBrandLogo(52);
    if (!logo_px.isNull()) {
        brand_logo->setPixmap(logo_px);
        brand_identity_layout->addWidget(brand_logo);
    }

    auto* brand_name = new QLabel(QStringLiteral("drippu"), brand_identity);
    brand_name->setStyleSheet(
        QStringLiteral("font-size:30pt; font-weight:800; color:#ffffff;"));
    brand_identity_layout->addWidget(brand_name);
    brand_layout->addWidget(brand_identity);

    auto* welcome = new QLabel(QStringLiteral("WELCOME TO DRIPPU"), brand_block);
    welcome->setObjectName(QStringLiteral("WelcomeLabel"));
    welcome->setAlignment(Qt::AlignCenter);
    brand_layout->addWidget(welcome);

    auto* title = new QLabel(QStringLiteral("Choose how you want to use the app"), brand_block);
    title->setObjectName(QStringLiteral("TitleLabel"));
    title->setAlignment(Qt::AlignCenter);
    brand_layout->addWidget(title);

    frame_layout->addWidget(brand_block);

    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(16);

    auto setupCard = [this](QPushButton* button, QStyle::StandardPixmap icon,
                            const QString& title_text, const QString& subtitle_text,
                            const QString& description, bool recommended) {
        button->setObjectName(QStringLiteral("ModeCard"));
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumSize(230, 260);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        button->setProperty("recommended", recommended);
        button->setAccessibleName(title_text);
        button->setAccessibleDescription(description);

        auto configureLabel = [](QLabel* label) {
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            label->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        };

        auto* card_layout = new QVBoxLayout(button);
        card_layout->setContentsMargins(22, 20, 22, 20);
        card_layout->setSpacing(7);

        auto* top_row = new QHBoxLayout();
        auto* icon_label = new QLabel(button);
        icon_label->setFixedSize(52, 52);
        icon_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        icon_label->setPixmap(style()->standardIcon(icon).pixmap(48, 48));
        configureLabel(icon_label);
        top_row->addWidget(icon_label);
        top_row->addStretch();
        if (recommended) {
            auto* badge = new QLabel(QStringLiteral("RECOMMENDED"), button);
            badge->setObjectName(QStringLiteral("CardBadge"));
            badge->setAlignment(Qt::AlignCenter);
            configureLabel(badge);
            top_row->addWidget(badge, 0, Qt::AlignTop);
        }
        card_layout->addLayout(top_row);

        auto* card_title = new QLabel(title_text, button);
        card_title->setObjectName(QStringLiteral("CardTitle"));
        configureLabel(card_title);
        card_layout->addWidget(card_title);

        auto* card_subtitle = new QLabel(subtitle_text, button);
        card_subtitle->setObjectName(QStringLiteral("CardSubtitle"));
        configureLabel(card_subtitle);
        card_layout->addWidget(card_subtitle);

        auto* card_description = new QLabel(description, button);
        card_description->setObjectName(QStringLiteral("CardDescription"));
        card_description->setWordWrap(true);
        card_description->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        configureLabel(card_description);
        card_layout->addWidget(card_description);
        card_layout->addStretch();

        auto* action = new QLabel(QStringLiteral("Choose %1  →").arg(title_text), button);
        action->setObjectName(QStringLiteral("CardAction"));
        configureLabel(action);
        card_layout->addWidget(action);
    };

    btn_gamer_ = new QPushButton(frame);
    setupCard(btn_gamer_, QStyle::SP_MediaPlay, QStringLiteral("Gamer"),
              QStringLiteral("Play and organize"), GamerDesc(), true);
    btn_gamer_->setDefault(true);
    connect(btn_gamer_, &QPushButton::clicked, this, &ModeSelector::OnGamerClicked);
    btn_layout->addWidget(btn_gamer_, 1);

    btn_programmer_ = new QPushButton(frame);
    setupCard(btn_programmer_, QStyle::SP_ComputerIcon, QStringLiteral("Programmer"),
              QStringLiteral("Build and test"), ProgrammerDesc(), false);
    connect(btn_programmer_, &QPushButton::clicked, this, &ModeSelector::OnProgrammerClicked);
    btn_layout->addWidget(btn_programmer_, 1);

    btn_hacker_ = new QPushButton(frame);
    setupCard(btn_hacker_, QStyle::SP_FileDialogDetailedView, QStringLiteral("Hacker"),
              QStringLiteral("Inspect and tune"), HackerDesc(), false);
    connect(btn_hacker_, &QPushButton::clicked, this, &ModeSelector::OnHackerClicked);
    btn_layout->addWidget(btn_hacker_, 1);

    frame_layout->addLayout(btn_layout, 1);

    auto* hint = new QFrame(frame);
    hint->setObjectName(QStringLiteral("ProfileHint"));
    auto* hint_layout = new QHBoxLayout(hint);
    hint_layout->setContentsMargins(14, 9, 14, 9);
    hint_layout->setSpacing(9);

    auto* hint_icon = new QLabel(hint);
    hint_icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(18, 18));
    hint_icon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    hint_layout->addWidget(hint_icon);
    auto* hint_text = new QLabel(
        QStringLiteral("You can switch profiles later from View → Change Interface Mode."), hint);
    hint_text->setObjectName(QStringLiteral("ProfileHintText"));
    hint_layout->addWidget(hint_text);
    hint_layout->addStretch();
    frame_layout->addWidget(hint);

    main_layout->addWidget(frame);
    btn_gamer_->setFocus(Qt::OtherFocusReason);
}

ModeSelector::~ModeSelector() = default;

void ModeSelector::ApplySelection(AppMode mode) {
    selected_mode_ = mode;
    remember_choice_ = true;

    if (remember_choice_) {
        SaveMode(mode);
    }
    accept();
}

void ModeSelector::OnGamerClicked() {
    ApplySelection(AppMode::Gamer);
}

void ModeSelector::OnProgrammerClicked() {
    ApplySelection(AppMode::Programmer);
}

void ModeSelector::OnHackerClicked() {
    ApplySelection(AppMode::Hacker);
}

AppMode ModeSelector::SelectedMode() const {
    return selected_mode_;
}

bool ModeSelector::RememberChoice() const {
    return remember_choice_;
}

AppMode ModeSelector::LoadSavedMode() {
    QSettings settings;
    const bool remember = settings.value(QLatin1String(kRememberKey), false).toBool();
    if (!remember) {
        return AppMode::Gamer;
    }
    const int val = settings.value(QLatin1String(kSettingsKey), 0).toInt();
    switch (val) {
    case 1:
        return AppMode::Programmer;
    case 2:
        return AppMode::Hacker;
    default:
        return AppMode::Gamer;
    }
}

void ModeSelector::SaveMode(AppMode mode) {
    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), static_cast<int>(mode));
    settings.setValue(QLatin1String(kRememberKey), true);
}
