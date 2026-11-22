// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/nxnative_export.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <memory>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <windows.h>
#endif

namespace {

QString ResourcePath(const QString& filename) {
#ifdef Q_OS_WIN
    return QStringLiteral(":/nxnative/") + filename;
#else
    return QDir(QApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../Resources/nxnative/") + filename);
#endif
}

#ifdef Q_OS_WIN
// The worker waits on stdin until it belongs to the job. Every compiler it then
// starts inherits the job, including when the user closes Drippu during a build.
class BuildJob {
public:
    bool Attach(qint64 pid) {
        Reset();
        handle = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!handle || !SetInformationJobObject(handle, JobObjectExtendedLimitInformation,
                                                 &limits, sizeof(limits))) {
            Reset();
            return false;
        }
        const auto child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                       static_cast<DWORD>(pid));
        const bool attached = child && AssignProcessToJobObject(handle, child);
        if (child) {
            CloseHandle(child);
        }
        if (!attached) {
            Reset();
        }
        return attached;
    }

    void Reset() {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    ~BuildJob() { Reset(); }

private:
    HANDLE handle{};
};
#endif

class NxNativeDialog final : public QDialog {
public:
    NxNativeDialog(QWidget* parent, const QString& game, quint64 title_id, const QString& output_dir,
                   const QString& keys_file)
        : QDialog(parent), base_path(game), program_id(title_id) {
#ifdef Q_OS_WIN
        setWindowTitle(tr("Build iPhone/iPad IPA with NX2Apple"));
#else
        setWindowTitle(tr("Build with NxNative"));
#endif
        resize(700, 550);
        auto* layout = new QVBoxLayout(this);
        auto* options = new QWidget(this);
        auto* form = new QFormLayout(options);
        layout->addWidget(options);

        auto* base = new QLabel(game, this);
        base->setWordWrap(true);
        base->setTextFormat(Qt::PlainText);
        form->addRow(tr("Base game"), base);
        target = new QComboBox(this);
#ifdef Q_OS_MACOS
        target->addItem(tr("Mac app (.app)"), QStringLiteral("mac-app"));
#endif
        target->addItem(tr("iPhone / iPad app (.ipa, unsigned)"), QStringLiteral("ios"));
        form->addRow(tr("Build for"), target);

        auto* addon_row = new QHBoxLayout;
        addons = new QListWidget(this);
        addons->setMaximumHeight(68);
        addons->setSelectionMode(QAbstractItemView::ExtendedSelection);
        addon_row->addWidget(addons);
        auto* add = new QPushButton(tr("Add files…"), this);
        auto* remove = new QPushButton(tr("Remove"), this);
        addon_row->addWidget(add);
        addon_row->addWidget(remove);
        form->addRow(tr("Update and DLC packages"), addon_row);
        connect(add, &QPushButton::clicked, this, [this] {
            const auto files = QFileDialog::getOpenFileNames(this, tr("Choose update and DLC packages"),
                QFileInfo(base_path).absolutePath(), tr("Switch packages (*.nsp *.xci *.nsz *.xcz)"));
            for (const auto& file : files) {
                if (file != base_path && addons->findItems(file, Qt::MatchExactly).isEmpty()) {
                    addons->addItem(file);
                }
            }
        });
        connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(addons->selectedItems()); });

        const auto path_row = [this, form](const QString& label, const QString& initial, bool directory,
                                           const QString& filter = {}) {
            auto* row = new QHBoxLayout;
            auto* edit = new QLineEdit(initial, this);
            auto* browse = new QPushButton(tr("Browse…"), this);
            row->addWidget(edit);
            row->addWidget(browse);
            form->addRow(label, row);
            connect(browse, &QPushButton::clicked, this, [this, edit, label, directory, filter] {
                const auto file = directory ? QFileDialog::getExistingDirectory(this, label, edit->text())
                                            : QFileDialog::getOpenFileName(this, label, edit->text(), filter, nullptr,
                                                                           QFileDialog::DontResolveSymlinks);
                if (!file.isEmpty()) {
                    edit->setText(file);
                }
            });
            return edit;
        };
        const QSettings previous;
#ifdef Q_OS_WIN
        checkout = path_row(tr("NX2Apple release folder"), previous.value("nxnative/checkout").toString(), true);
        const auto nx_python = QDir(qEnvironmentVariable("LOCALAPPDATA"))
                                   .filePath(QStringLiteral("NS Port Builder/venv/Scripts/python.exe"));
        const auto default_python = QFileInfo(nx_python).isFile() ? nx_python
            : QStandardPaths::findExecutable(QStringLiteral("python.exe"));
        python = path_row(tr("Python executable"), previous.value("nxnative/python", default_python).toString(), false);
#else
        auto* toolchain = new QComboBox(this);
        toolchain->addItem(tr("NxNative checkout"));
        toolchain->addItem(tr("NX2Apple 1.5.0 for Mac"));
        form->addRow(tr("Toolchain"), toolchain);
        const auto checkout_row = form->rowCount();
        checkout = path_row(tr("NxNative checkout"), previous.value("nxnative/checkout").toString(), true);
        python = path_row(tr("Python executable"), previous.value("nxnative/python",
                          QStandardPaths::findExecutable(QStringLiteral("python3"))).toString(), false);
        connect(checkout, &QLineEdit::textChanged, this, [this](const QString& path) {
            const auto venv = QDir(path).absoluteFilePath(QStringLiteral(".venv/bin/python"));
            if (QFileInfo(venv).isExecutable()) {
                python->setText(venv);
            }
        });
        const auto app_row = form->rowCount();
        const auto applications = tr("Mac applications (*.app)");
        release_app = path_row(tr("NX2Apple app"), previous.value("nxnative/macRelease",
                               QStringLiteral("/Applications/NX2Apple.app")).toString(), false, applications);
        xcode = path_row(tr("Xcode app"), previous.value("nxnative/xcode").toString(), false, applications);
        xcode->setPlaceholderText(tr("Use system Xcode"));
        const auto select_mode = [this, form, checkout_row, app_row](int index) {
            using_release = index == 1;
            form->setRowVisible(checkout_row, !using_release);
            form->setRowVisible(checkout_row + 1, !using_release);
            form->setRowVisible(app_row, using_release);
        };
        toolchain->setCurrentIndex(previous.value("nxnative/useMacRelease",
            checkout->text().isEmpty() && QFileInfo(release_app->text()).isDir()).toBool() ? 1 : 0);
        select_mode(toolchain->currentIndex());
        connect(toolchain, &QComboBox::currentIndexChanged, this, select_mode);
#endif
        keys = path_row(tr("Console keys"), previous.value("nxnative/keys", keys_file).toString(), false);
        output = path_row(tr("Export folder"), output_dir, true);

        auto* buttons = new QHBoxLayout;
        auto* setup = new QPushButton(tr("Setup…"), this);
        notes = new QPushButton(tr("Upstream game notes…"), this);
        notes->setEnabled(false);
#ifdef Q_OS_WIN
        reveal = new QPushButton(tr("Open export folder"), this);
#else
        reveal = new QPushButton(tr("Show in Finder"), this);
#endif
        reveal->setEnabled(false);
        buttons->addWidget(setup);
        buttons->addWidget(notes);
        buttons->addWidget(reveal);
        buttons->addStretch();
        layout->addLayout(buttons);
        connect(setup, &QPushButton::clicked, this, [this] {
            ShowDocument(tr("Build setup"), ResourcePath(QStringLiteral("setup.md")));
        });
        connect(notes, &QPushButton::clicked, this, [this] {
            ShowDocument(tr("Upstream game notes"), notes_path);
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
#ifdef Q_OS_WIN
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(artifact).absolutePath()));
#else
            QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), artifact});
#endif
        });

        log = new QPlainTextEdit(this);
        log->setReadOnly(true);
        log->setMaximumBlockCount(3000);
        log->setPlaceholderText(tr("Check the selected packages and toolchain before building. "
                                   "Only the game and add-on files selected here are included."));
        layout->addWidget(log, 1);
        status = new QLabel(this);
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* actions = new QHBoxLayout;
        actions->addStretch();
        stop = new QPushButton(tr("Cancel"), this);
        stop->setEnabled(false);
        action = new QPushButton(tr("Check compatibility"), this);
        actions->addWidget(stop);
        actions->addWidget(action);
        layout->addLayout(actions);

        process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
        connect(&process, &QProcess::started, this, [this] {
            if (cancelled || !job.Attach(process.processId())) {
                if (!cancelled) {
                    log->appendPlainText(tr("Windows could not create an isolated build process."));
                }
                process.kill();
                return;
            }
            process.write("start\n");
        });
#endif
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { ReadLines(false); });
        auto restore = [this, options] {
#ifdef Q_OS_WIN
            job.Reset();
            if (scratch && !scratch->remove()) {
                log->appendPlainText(tr("Temporary files could not be removed from %1.").arg(scratch->path()));
            }
            scratch.reset();
            worker_script.reset();
#endif
            options->setEnabled(true);
            stop->setEnabled(false);
            action->setText(tr("Check compatibility"));
            action->setEnabled(true);
            ready = false;
        };
        connect(&process, &QProcess::errorOccurred, this, [this, restore](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                restore();
                status->setText(tr("Could not start Python. Choose the executable from Setup."));
                log->appendPlainText(process.errorString());
            }
        });
        connect(&process, &QProcess::finished, this, [this, restore](int code, QProcess::ExitStatus exit) {
            ReadLines(true);
            restore();
            if (code == 0 && exit == QProcess::NormalExit && !artifact.isEmpty()) {
                reveal->setEnabled(true);
                status->setText(target->currentData().toString() == QStringLiteral("ios")
                        ? tr("IPA ready. Sign and install it with your sideloading tool.")
                        : tr("Mac app ready. %1").arg(verification));
                log->appendPlainText(artifact);
            } else {
                status->setText(cancelled ? tr("Cancelled.") : tr("Export stopped. See the log above."));
            }
        });
        connect(stop, &QPushButton::clicked, this, [this] { Cancel(); });
        connect(action, &QPushButton::clicked, this, [this, options] {
            if (ready) {
                ready = false;
                action->setEnabled(false);
                status->setText(tr("Building…"));
                process.write("build\n");
                process.closeWriteChannel();
                return;
            }
            QString installation = checkout->text();
            QString interpreter = python->text();
            QSettings settings;
#ifdef Q_OS_MACOS
            settings.setValue("nxnative/useMacRelease", using_release);
            settings.setValue("nxnative/macRelease", release_app->text());
            settings.setValue("nxnative/xcode", xcode->text());
            if (using_release) {
                installation = release_app->text();
                interpreter = QDir(installation).filePath(QStringLiteral("Contents/Resources/python/bin/python3"));
                if (!QFileInfo(interpreter).isExecutable()) {
                    status->setText(tr("Choose the NX2Apple release app containing its bundled Python."));
                    return;
                }
            }
#endif
            if (installation.isEmpty() || interpreter.isEmpty() || keys->text().isEmpty() || output->text().isEmpty()) {
                status->setText(tr("Choose the build tools, console keys, and export folder."));
                return;
            }
            settings.setValue("nxnative/checkout", checkout->text());
            settings.setValue("nxnative/python", python->text());
            settings.setValue("nxnative/keys", keys->text());
            QString script = ResourcePath(QStringLiteral("export.py"));
            const QString destination = QFileInfo(output->text()).absoluteFilePath();
#ifdef Q_OS_WIN
            if (!QDir().mkpath(destination)) {
                status->setText(tr("Could not create the export folder."));
                return;
            }
            scratch = std::make_unique<QTemporaryDir>(QDir(destination).filePath(QStringLiteral(".drippu-nx-XXXXXX")));
            worker_script.reset(QTemporaryFile::createNativeFile(script));
            if (!scratch->isValid() || !worker_script) {
                scratch.reset();
                worker_script.reset();
                status->setText(tr("Could not create temporary build files."));
                return;
            }
            script = worker_script->fileName();
            worker_script->close();
#endif
            QStringList args{QStringLiteral("-u"), script, QStringLiteral("--checkout"),
                             QFileInfo(installation).absoluteFilePath(), QStringLiteral("--keys"),
                             QFileInfo(keys->text()).absoluteFilePath(), QStringLiteral("--output"),
                             destination, QStringLiteral("--target"), target->currentData().toString()};
#ifdef Q_OS_WIN
            args << QStringLiteral("--staging") << scratch->path();
#else
            if (using_release) {
                args.prepend(QStringLiteral("-B"));
                args.prepend(QStringLiteral("-I"));
                args << QStringLiteral("--nx2apple");
            }
            if (!xcode->text().isEmpty()) {
                args << QStringLiteral("--xcode") << QFileInfo(xcode->text()).absoluteFilePath();
            }
#endif
            if (program_id) {
                args << QStringLiteral("--title") << QString::number(program_id, 16);
            }
            args << QStringLiteral("--") << QFileInfo(base_path).absoluteFilePath();
            for (int i = 0; i < addons->count(); ++i) {
                args << addons->item(i)->text();
            }
            options->setEnabled(false);
            action->setEnabled(false);
            stop->setEnabled(true);
            reveal->setEnabled(false);
            notes->setEnabled(false);
            cancelled = false;
            artifact.clear();
            verification.clear();
            pending.clear();
            log->clear();
            status->setText(tr("Checking packages and build tools…"));
            process.start(interpreter, args);
        });
    }

protected:
    void reject() override {
        if (process.state() != QProcess::NotRunning) {
            Cancel();
            return;
        }
        QDialog::reject();
    }

private:
    void Cancel() {
        cancelled = true;
        stop->setEnabled(false);
        action->setEnabled(false);
        status->setText(tr("Stopping…"));
#ifdef Q_OS_WIN
        job.Reset();
        process.kill(); // Also covers cancellation before the startup handshake.
#else
        process.terminate();
#endif
    }

    void ShowDocument(const QString& title, const QString& path) {
        QDialog dialog(this);
        dialog.setWindowTitle(title);
        dialog.resize(720, 500);
        auto* box = new QVBoxLayout(&dialog);
        auto* document = new QTextBrowser(&dialog);
        document->setOpenExternalLinks(true);
        const auto url = path.startsWith(QStringLiteral(":/")) ? QUrl(QStringLiteral("qrc") + path)
                                                              : QUrl::fromLocalFile(path);
        document->setSource(url, QTextDocument::MarkdownResource);
        box->addWidget(document);
        dialog.exec();
    }

    void ReadLines(bool finished) {
        pending += process.readAllStandardOutput();
        qsizetype end;
        while ((end = pending.indexOf('\n')) >= 0 || (finished && !pending.isEmpty())) {
            const auto line = end < 0 ? pending : pending.left(end);
            pending.remove(0, end < 0 ? pending.size() : end + 1);
            const auto obj = QJsonDocument::fromJson(line).object();
            const auto ev = obj[QStringLiteral("ev")].toString();
            if (ev == QStringLiteral("drippu_ready") && !cancelled) {
                log->appendPlainText(obj[QStringLiteral("msg")].toString());
                notes_path = obj[QStringLiteral("notes")].toString();
                notes->setEnabled(QFileInfo(notes_path).isFile());
                status->setText(tr("Review the selected version and platform evidence before building."));
                ready = true;
                action->setText(tr("Build"));
                action->setEnabled(true);
            } else if (ev == QStringLiteral("drippu_product")) {
                artifact = obj[QStringLiteral("path")].toString();
                verification = obj[QStringLiteral("verification")].toString();
            } else if (ev == QStringLiteral("say")) {
                log->appendPlainText(obj[QStringLiteral("msg")].toString());
            } else if (ev == QStringLiteral("stage") || ev == QStringLiteral("step") || ev == QStringLiteral("progress")) {
                const auto stage = obj[QStringLiteral("name")].toString() + QStringLiteral(" — ") + obj[QStringLiteral("step")].toString();
                if (!cancelled) {
                    status->setText(stage);
                }
                if (ev == QStringLiteral("stage")) {
                    log->appendPlainText(stage);
                }
            } else if (ev == QStringLiteral("stage_end")) {
                log->appendPlainText(obj[QStringLiteral("summary")].toString());
            } else if (ev.isEmpty() && !line.trimmed().isEmpty()) {
                log->appendPlainText(QString::fromUtf8(line));
            }
        }
    }

    QString base_path, notes_path, artifact, verification;
    quint64 program_id;
    QComboBox* target;
    QListWidget* addons;
    QLineEdit *checkout, *python, *keys, *output;
#ifdef Q_OS_MACOS
    QLineEdit *release_app, *xcode;
    bool using_release = false;
#endif
    QPushButton *action, *stop, *notes, *reveal;
    QPlainTextEdit* log;
    QLabel* status;
#ifdef Q_OS_WIN
    std::unique_ptr<QTemporaryDir> scratch;
    std::unique_ptr<QTemporaryFile> worker_script;
    BuildJob job;
#endif
    QProcess process;
    QByteArray pending;
    bool ready = false;
    bool cancelled = false;
};

} // namespace

void ShowNxNativeExport(QWidget* parent, const QString& game, quint64 title_id,
                        const QString& output_dir, const QString& keys_file) {
    NxNativeDialog dialog(parent, game, title_id, output_dir, keys_file);
    dialog.exec();
}
