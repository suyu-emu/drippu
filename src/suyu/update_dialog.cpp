// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/update_dialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "common/scm_rev.h"
#include "common/settings.h"

UpdateDialog::UpdateDialog(QWidget* parent, std::function<void()> save_callback_)
    : QDialog(parent), save_callback(std::move(save_callback_)) {
    setWindowTitle(tr("Check for Updates"));
    setMinimumWidth(560);
    BuildUi();
    LoadSourcesIntoUi();

    watcher = new QFutureWatcher<std::vector<UpdateChecker::SourcedRelease>>(this);
    connect(watcher, &QFutureWatcher<std::vector<UpdateChecker::SourcedRelease>>::finished, this,
            &UpdateDialog::OnCheckFinished);
}

UpdateDialog::~UpdateDialog() = default;

void UpdateDialog::BuildUi() {
    auto* layout = new QVBoxLayout(this);

    version_label = new QLabel(
        tr("Current version: %1").arg(QString::fromStdString(Common::g_build_fullname)), this);
    layout->addWidget(version_label);

    auto* check_row = new QHBoxLayout;
    status_label = new QLabel(tr("Not checked yet."), this);
    status_label->setWordWrap(true);
    check_row->addWidget(status_label, 1);
    check_button = new QPushButton(tr("Check Now"), this);
    connect(check_button, &QPushButton::clicked, this, &UpdateDialog::CheckNow);
    check_row->addWidget(check_button);
    layout->addLayout(check_row);

    results_list = new QListWidget(this);
    connect(results_list, &QListWidget::currentItemChanged, this,
            &UpdateDialog::OnSelectionChanged);
    layout->addWidget(results_list, 1);

    auto* action_row = new QHBoxLayout;
    open_page_button = new QPushButton(tr("Open Release Page"), this);
    open_page_button->setEnabled(false);
    connect(open_page_button, &QPushButton::clicked, this, &UpdateDialog::OnOpenReleasePage);
    action_row->addWidget(open_page_button);
    download_button = new QPushButton(tr("Download"), this);
    download_button->setEnabled(false);
    download_button->setToolTip(
        tr("Opens the first matching installable for this platform in the browser."));
    connect(download_button, &QPushButton::clicked, this, &UpdateDialog::OnDownloadAsset);
    action_row->addWidget(download_button);
    action_row->addStretch(1);
    layout->addLayout(action_row);

    // --- Sources ---
    auto* sources_box = new QGroupBox(tr("Update sources"), this);
    auto* sources_layout = new QVBoxLayout(sources_box);

    startup_checkbox = new QCheckBox(tr("Check for updates on startup"), this);
    sources_layout->addWidget(startup_checkbox);

    auto* primary_label = new QLabel(
        tr("Primary source (leave all four empty to use the built-in default):"), this);
    sources_layout->addWidget(primary_label);

    repo_edit = new QLineEdit(this);
    repo_edit->setPlaceholderText(tr("owner/repo, e.g. suyu-emu/drippu"));
    sources_layout->addWidget(repo_edit);
    api_host_edit = new QLineEdit(this);
    api_host_edit->setPlaceholderText(tr("API host, e.g. api.github.com"));
    sources_layout->addWidget(api_host_edit);
    api_path_edit = new QLineEdit(this);
    api_path_edit->setPlaceholderText(tr("Latest-release path, e.g. /repos/owner/repo/releases/latest"));
    sources_layout->addWidget(api_path_edit);
    website_edit = new QLineEdit(this);
    website_edit->setPlaceholderText(tr("Website, e.g. https://github.com"));
    sources_layout->addWidget(website_edit);

    auto* extras_label = new QLabel(
        tr("Additional sources (checked alongside the primary). Add an owner/repo "
           "slug, or paste a full source line: name|api_host|api_path|repo|website"),
        this);
    extras_label->setWordWrap(true);
    sources_layout->addWidget(extras_label);

    extras_list = new QListWidget(this);
    extras_list->setMaximumHeight(110);
    sources_layout->addWidget(extras_list);

    auto* extra_row = new QHBoxLayout;
    extra_add_edit = new QLineEdit(this);
    extra_add_edit->setPlaceholderText(tr("owner/repo or name|api_host|api_path|repo|website"));
    extra_row->addWidget(extra_add_edit, 1);
    auto* add_button = new QPushButton(tr("Add"), this);
    connect(add_button, &QPushButton::clicked, this, &UpdateDialog::OnAddExtra);
    extra_row->addWidget(add_button);
    auto* remove_button = new QPushButton(tr("Remove"), this);
    connect(remove_button, &QPushButton::clicked, this, &UpdateDialog::OnRemoveExtra);
    extra_row->addWidget(remove_button);
    sources_layout->addLayout(extra_row);

    layout->addWidget(sources_box);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &UpdateDialog::OnSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void UpdateDialog::LoadSourcesIntoUi() {
    startup_checkbox->setChecked(Settings::values.update_check_startup.GetValue());
    repo_edit->setText(QString::fromStdString(Settings::values.update_repo_override.GetValue()));
    api_host_edit->setText(QString::fromStdString(Settings::values.update_api_host.GetValue()));
    api_path_edit->setText(QString::fromStdString(Settings::values.update_api_path.GetValue()));
    website_edit->setText(QString::fromStdString(Settings::values.update_website.GetValue()));

    // Show the effective values as placeholder hints where the user left a
    // field empty, so "empty means default" is visible rather than implied.
    const auto def = Common::Net::UpdateSource::Default();
    if (repo_edit->text().isEmpty()) {
        repo_edit->setPlaceholderText(
            tr("owner/repo (default: %1)").arg(QString::fromStdString(def.repo)));
    }
    if (api_host_edit->text().isEmpty()) {
        api_host_edit->setPlaceholderText(
            tr("API host (default: %1)").arg(QString::fromStdString(def.api_host)));
    }

    extras_list->clear();
    for (const auto& extra :
         Common::Net::ParseExtraSources(Settings::values.update_extra_sources.GetValue())) {
        extras_list->addItem(QString::fromStdString(extra.Serialize()));
    }
}

void UpdateDialog::CheckNow() {
    if (checking) {
        return;
    }
    checking = true;
    check_button->setEnabled(false);
    status_label->setText(tr("Checking %1 source(s)...")
                              .arg(Common::Net::ResolveUpdateSources().size()));
    results_list->clear();
    open_page_button->setEnabled(false);
    download_button->setEnabled(false);
    watcher->setFuture(QtConcurrent::run(&UpdateChecker::GetLatestPerSource));
}

void UpdateDialog::OnCheckFinished() {
    checking = false;
    check_button->setEnabled(true);
    last_results = watcher->result();

    results_list->clear();
    if (last_results.empty()) {
        status_label->setText(tr("No source answered. Check the source settings below - "
                                 "an empty primary with no compiled-in default checks nothing."));
        return;
    }

    for (const auto& row : last_results) {
        auto* item = new QListWidgetItem(Describe(row), results_list);
        item->setData(Qt::UserRole, QString::fromStdString(row.release.tag));
    }

    // Newest among the already-fetched rows that is not this build. No second
    // network round: GetBestUpdate() would re-fetch every source on the UI
    // thread, which is exactly what the background check was avoiding.
    const UpdateChecker::SourcedRelease* best = nullptr;
    for (const auto& row : last_results) {
        if (!UpdateChecker::IsDifferentFromBuild(row.release.tag)) {
            continue;
        }
        if (best == nullptr || row.release.published > best->release.published) {
            best = &row;
        }
    }

    if (best != nullptr) {
        status_label->setText(tr("Update available: %1 (from %2).")
                                  .arg(QString::fromStdString(best->release.tag),
                                       QString::fromStdString(best->source_name)));
    } else {
        status_label->setText(tr("Up to date on all %1 source(s).").arg(last_results.size()));
    }
}

QString UpdateDialog::Describe(const UpdateChecker::SourcedRelease& row) {
    const QString date = row.release.published != 0
                             ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(row.release.published))
                                   .toString(Qt::ISODate)
                             : tr("unknown date");
    return tr("%1 — %2 (%3)")
        .arg(QString::fromStdString(row.source_name), QString::fromStdString(row.release.tag),
             date);
}

void UpdateDialog::OnSelectionChanged() {
    const bool has = results_list->currentRow() >= 0 &&
                     results_list->currentRow() < static_cast<int>(last_results.size());
    open_page_button->setEnabled(has);
    download_button->setEnabled(has && !DownloadUrlFor(last_results[results_list->currentRow()].release).isEmpty());
}

QString UpdateDialog::DownloadUrlFor(const Common::Net::Release& release) {
    const auto assets = release.GetPlatformAssets();
    if (assets.empty()) {
        return {};
    }
    const QString path = QString::fromStdString(assets.front().path);
    if (path.startsWith(QStringLiteral("https://")) || path.startsWith(QStringLiteral("http://"))) {
        return path;
    }
    return QString::fromStdString(assets.front().url) + path;
}

void UpdateDialog::OnOpenReleasePage() {
    const int row = results_list->currentRow();
    if (row < 0 || row >= static_cast<int>(last_results.size())) {
        return;
    }
    QDesktopServices::openUrl(QUrl(QString::fromStdString(last_results[row].release.html_url)));
}

void UpdateDialog::OnDownloadAsset() {
    const int row = results_list->currentRow();
    if (row < 0 || row >= static_cast<int>(last_results.size())) {
        return;
    }
    const QString url = DownloadUrlFor(last_results[row].release);
    if (url.isEmpty()) {
        QMessageBox::information(this, tr("Download"),
                                 tr("No installable found for this platform in that release."));
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}

void UpdateDialog::OnAddExtra() {
    const QString text = extra_add_edit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    Common::Net::UpdateSource source;
    if (text.contains(QLatin1Char('|'))) {
        auto parsed = Common::Net::UpdateSource::Deserialize(text.toStdString());
        if (!parsed) {
            QMessageBox::warning(this, tr("Add source"),
                                 tr("That line is not a valid source. Expected "
                                    "name|api_host|api_path|repo|website"));
            return;
        }
        source = std::move(parsed.value());
    } else {
        // Bare "owner/repo" slug. Minimal validation: exactly one slash, no
        // whitespace, nothing that looks like a URL scheme.
        static const QRegularExpression slug_re(QStringLiteral("^[^\\s/]+/[^\\s/]+$"));
        if (!slug_re.match(text).hasMatch() || text.contains(QStringLiteral("://"))) {
            QMessageBox::warning(this, tr("Add source"),
                                 tr("Expected an owner/repo slug (e.g. suyu-emu/drippu)."));
            return;
        }
        source = Common::Net::UpdateSource::FromRepoSlug(text.toStdString());
    }
    extras_list->addItem(QString::fromStdString(source.Serialize()));
    extra_add_edit->clear();
}

void UpdateDialog::OnRemoveExtra() {
    delete extras_list->takeItem(extras_list->currentRow());
}

void UpdateDialog::OnSave() {
    Settings::values.update_check_startup.SetValue(startup_checkbox->isChecked());
    Settings::values.update_repo_override.SetValue(repo_edit->text().trimmed().toStdString());
    Settings::values.update_api_host.SetValue(api_host_edit->text().trimmed().toStdString());
    Settings::values.update_api_path.SetValue(api_path_edit->text().trimmed().toStdString());
    Settings::values.update_website.SetValue(website_edit->text().trimmed().toStdString());

    std::string extras;
    for (int i = 0; i < extras_list->count(); ++i) {
        if (i > 0) {
            extras += '\n';
        }
        extras += extras_list->item(i)->text().toStdString();
    }
    // Validate before persisting: drop anything malformed rather than saving a
    // setting that will warn on every future load.
    const auto parsed = Common::Net::ParseExtraSources(extras);
    std::string clean;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        if (i > 0) {
            clean += '\n';
        }
        clean += parsed[i].Serialize();
    }
    Settings::values.update_extra_sources.SetValue(std::move(clean));

    if (save_callback) {
        save_callback();
    }
    accept();
}
