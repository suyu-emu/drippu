// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/nxnative_export.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
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

namespace {

QString ResourcePath(const QString& filename) {
    return QDir(QApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../Resources/nxnative/") + filename);
}

class NxNativeDialog final : public QDialog {
public:
    NxNativeDialog(QWidget* parent, const QString& game, quint64 title_id, const QString& output_dir,
                   const QString& keys_file)
        : QDialog(parent), base_path(game), program_id(title_id) {
        setWindowTitle(tr("Build with NxNative"));
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
        target->addItem(tr("Mac app (.app)"), QStringLiteral("mac-app"));
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

        const auto path_row = [this, form](const QString& label, const QString& initial, bool directory) {
            auto* row = new QHBoxLayout;
            auto* edit = new QLineEdit(initial, this);
            auto* browse = new QPushButton(tr("Browse…"), this);
            row->addWidget(edit);
            row->addWidget(browse);
            form->addRow(label, row);
            connect(browse, &QPushButton::clicked, this, [this, edit, label, directory] {
                const auto file = directory ? QFileDialog::getExistingDirectory(this, label, edit->text())
                                            : QFileDialog::getOpenFileName(this, label, edit->text(), {}, nullptr,
                                                                           QFileDialog::DontResolveSymlinks);
                if (!file.isEmpty()) {
                    edit->setText(file);
                }
            });
            return edit;
        };
        const QSettings previous;
        checkout = path_row(tr("NxNative checkout"), previous.value("nxnative/checkout").toString(), true);
        python = path_row(tr("Python executable"), previous.value("nxnative/python",
                          QStandardPaths::findExecutable(QStringLiteral("python3"))).toString(), false);
        connect(checkout, &QLineEdit::textChanged, this, [this](const QString& path) {
            const auto venv = QDir(path).absoluteFilePath(QStringLiteral(".venv/bin/python"));
            if (QFileInfo(venv).isExecutable()) {
                python->setText(venv);
            }
        });
        keys = path_row(tr("Console keys"), previous.value("nxnative/keys", keys_file).toString(), false);
        output = path_row(tr("Export folder"), output_dir, true);

        auto* buttons = new QHBoxLayout;
        auto* setup = new QPushButton(tr("Setup…"), this);
        notes = new QPushButton(tr("Upstream game notes…"), this);
        notes->setEnabled(false);
        reveal = new QPushButton(tr("Show in Finder"), this);
        reveal->setEnabled(false);
        buttons->addWidget(setup);
        buttons->addWidget(notes);
        buttons->addWidget(reveal);
        buttons->addStretch();
        layout->addLayout(buttons);
        connect(setup, &QPushButton::clicked, this, [this] {
            ShowDocument(tr("NxNative setup"), ResourcePath(QStringLiteral("setup.md")));
        });
        connect(notes, &QPushButton::clicked, this, [this] {
            ShowDocument(tr("Upstream game notes"), notes_path);
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
            QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), artifact});
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
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { ReadLines(false); });
        auto restore = [this, options] {
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
                        : tr("Mac app ready."));
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
            if (checkout->text().isEmpty() || python->text().isEmpty() || keys->text().isEmpty() ||
                output->text().isEmpty()) {
                status->setText(tr("Choose a checkout, Python executable, keys file, and export folder."));
                return;
            }
            QSettings settings;
            settings.setValue("nxnative/checkout", checkout->text());
            settings.setValue("nxnative/python", python->text());
            settings.setValue("nxnative/keys", keys->text());
            QStringList args{QStringLiteral("-u"), ResourcePath(QStringLiteral("export.py")), QStringLiteral("--checkout"), checkout->text(),
                             QStringLiteral("--keys"), keys->text(), QStringLiteral("--output"), output->text(), QStringLiteral("--target"),
                             target->currentData().toString()};
            if (program_id) {
                args << QStringLiteral("--title") << QString::number(program_id, 16);
            }
            args << QStringLiteral("--") << base_path;
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
            pending.clear();
            log->clear();
            status->setText(tr("Checking packages and Apple build tools…"));
            process.start(python->text(), args);
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
        process.terminate();
    }

    void ShowDocument(const QString& title, const QString& path) {
        QDialog dialog(this);
        dialog.setWindowTitle(title);
        dialog.resize(720, 500);
        auto* box = new QVBoxLayout(&dialog);
        auto* document = new QTextBrowser(&dialog);
        document->setOpenExternalLinks(true);
        document->setSource(QUrl::fromLocalFile(path), QTextDocument::MarkdownResource);
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

    QString base_path, notes_path, artifact;
    quint64 program_id;
    QComboBox* target;
    QListWidget* addons;
    QLineEdit *checkout, *python, *keys, *output;
    QPushButton *action, *stop, *notes, *reveal;
    QPlainTextEdit* log;
    QLabel* status;
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
