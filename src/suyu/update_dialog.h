// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QDialog>

#include "common/net/net.h"
#include "frontend_common/update_checker.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
template <typename T>
class QFutureWatcher;

/**
 * Self-update dialog: checks every configured update source for a newer
 * release and lets the user attach custom ones.
 *
 * Sources are Common::Net::UpdateSource entries: the compiled-in default
 * (this project's own releases) unless overridden, plus any extras. A source
 * is five strings, so a fork, a mirror, or a whole new home for drippu is
 * describable - including non-GitHub hosts, which answer the same endpoints.
 *
 * Network checks run off the UI thread (QtConcurrent + QFutureWatcher). This
 * dialog only checks and points at downloads; it never replaces the running
 * binary - installing an update is platform-specific (installer, package
 * manager, AppImage swap) and doing it wrong bricks the install.
 */
class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    /// @param save_callback persists Settings after the user edits sources
    ///        (GMainWindow passes config->SaveAllValues).
    explicit UpdateDialog(QWidget* parent, std::function<void()> save_callback);
    ~UpdateDialog() override;

    /// Run a check immediately (used by the Help-menu action and the startup
    /// check) instead of waiting for the user to press the button.
    void CheckNow();

private:
    void BuildUi();
    void LoadSourcesIntoUi();
    void OnCheckFinished();
    void OnSelectionChanged();
    void OnSave();
    void OnAddExtra();
    void OnRemoveExtra();
    void OnOpenReleasePage();
    void OnDownloadAsset();

    /// Download URL for a release: the first platform asset, preferring a
    /// fully-qualified URL (GitHub API form) and falling back to host + path
    /// (custom-API form).
    static QString DownloadUrlFor(const Common::Net::Release& release);
    static QString Describe(const UpdateChecker::SourcedRelease& row);

    std::function<void()> save_callback;
    bool checking = false;

    QLabel* version_label{};
    QLabel* status_label{};
    QPushButton* check_button{};
    QListWidget* results_list{};
    QPushButton* open_page_button{};
    QPushButton* download_button{};

    QCheckBox* startup_checkbox{};
    QLineEdit* repo_edit{};
    QLineEdit* api_host_edit{};
    QLineEdit* api_path_edit{};
    QLineEdit* website_edit{};
    QListWidget* extras_list{};
    QLineEdit* extra_add_edit{};

    std::vector<UpdateChecker::SourcedRelease> last_results;
    QFutureWatcher<std::vector<UpdateChecker::SourcedRelease>>* watcher{};
};
