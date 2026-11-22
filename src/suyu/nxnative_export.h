// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

class QWidget;

void ShowNxNativeExport(QWidget* parent, const QString& game, quint64 title_id,
                        const QString& output_dir, const QString& keys_file);
