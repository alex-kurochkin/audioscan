#include "SettingsDialog.h"
#include "Settings.h"
#include "Database.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QFileDialog>
#include <QKeySequenceEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QMessageBox>
#include <QDir>

void SettingsDialog::buildUi() {
    auto &S = Settings::instance();
    setWindowTitle(tr("Settings"));
    resize(560, 620);
    auto *main = new QVBoxLayout(this);

    // ---- Storage maintenance ----
    {
        auto *box = new QGroupBox(tr("Storage maintenance"), this);
        auto *lay = new QHBoxLayout(box);
        auto *delMarkers = new QPushButton(tr("Delete all markers"), box);
        auto *scanMarkers = new QPushButton(tr("Scan markers"), box);
        auto *delPreviews = new QPushButton(tr("Delete all previews"), box);
        lay->addWidget(delMarkers);
        lay->addWidget(scanMarkers);
        lay->addWidget(delPreviews);
        main->addWidget(box);

        connect(delMarkers, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this, tr("Delete all markers"),
                    tr("Remove ALL markers from the database?")) == QMessageBox::Yes)
                Database::instance().clearAllMarkers();
        });
        connect(scanMarkers, &QPushButton::clicked, this, [this, &S] {
            int removed = Database::instance().scanMarkers();
            QMessageBox::information(this, tr("Scan markers"),
                tr("%1 stale marker record(s) removed.").arg(removed));
            Q_UNUSED(S);
        });
        connect(delPreviews, &QPushButton::clicked, this, [this, &S] {
            Database::instance().clearAllPreviews();
            QDir d(S.previewCacheDir());
            const auto files = d.entryList(QDir::Files);
            for (const QString &f : files) d.remove(f);
            QMessageBox::information(this, tr("Previews"), tr("Preview cache cleared."));
        });
    }

    // ---- Undo parameters ----
    {
        auto *box = new QGroupBox(tr("Undo history"), this);
        auto *form = new QFormLayout(box);
        auto *depth = new QSpinBox(box);
        depth->setRange(1, 200);
        depth->setValue(S.undoDepth());
        form->addRow(tr("History depth (steps):"), depth);

        auto *disk = new QSpinBox(box);
        disk->setRange(16, 1024 * 1024);
        disk->setValue(int(S.undoDiskLimitBytes() / (1024 * 1024)));
        disk->setSuffix(QStringLiteral(" MB"));
        form->addRow(tr("Disk limit:"), disk);

        auto *dirEdit = new QLineEdit(S.undoDir(), box);
        auto *browse = new QPushButton(tr("Browse..."), box);
        auto *dirRow = new QHBoxLayout;
        dirRow->addWidget(dirEdit); dirRow->addWidget(browse);
        form->addRow(tr("Undo data directory:"), dirRow);

        main->addWidget(box);
        connect(depth, qOverload<int>(&QSpinBox::valueChanged), this,
                [&S](int v) { S.setUndoDepth(v); });
        connect(disk, qOverload<int>(&QSpinBox::valueChanged), this,
                [&S](int v) { S.setUndoDiskLimit(qint64(v) * 1024 * 1024); });
        connect(browse, &QPushButton::clicked, this, [this, dirEdit] {
            QString d = QFileDialog::getExistingDirectory(this, tr("Undo directory"), dirEdit->text());
            if (!d.isEmpty()) dirEdit->setText(d);
        });
        connect(dirEdit, &QLineEdit::editingFinished, this, [dirEdit, &S] {
            S.setUndoDir(dirEdit->text());
        });
    }

    // ---- Behavior / appearance ----
    {
        auto *box = new QGroupBox(tr("Behavior and appearance"), this);
        auto *form = new QFormLayout(box);

        auto *prefetch = new QSpinBox(box);
        prefetch->setRange(0, 1000);
        prefetch->setValue(S.previewPrefetchCount());
        form->addRow(tr("Preview prefetch count:"), prefetch);

        auto *space = new QComboBox(box);
        space->addItem(tr("Start/Stop"), true);
        space->addItem(tr("Start/Pause"), false);
        space->setCurrentIndex(S.spaceStartStop() ? 0 : 1);
        form->addRow(tr("Space key behavior:"), space);

        auto *tblMode = new QComboBox(box);
        tblMode->addItem(tr("Waveform"), QStringLiteral("wave"));
        tblMode->addItem(tr("Spectrogram"), QStringLiteral("spectrogram"));
        tblMode->setCurrentIndex(S.tablePreviewMode() == QLatin1String("spectrogram") ? 1 : 0);
        form->addRow(tr("Table preview mode:"), tblMode);

        auto *plMode = new QComboBox(box);
        plMode->addItem(tr("Waveform"), QStringLiteral("wave"));
        plMode->addItem(tr("Spectrogram"), QStringLiteral("spectrogram"));
        plMode->setCurrentIndex(S.playerMode() == QLatin1String("spectrogram") ? 1 : 0);
        form->addRow(tr("Player display mode:"), plMode);

        main->addWidget(box);
        connect(prefetch, qOverload<int>(&QSpinBox::valueChanged), this,
                [&S](int v) { S.setPreviewPrefetchCount(v); });
        connect(space, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [space, &S](int) { S.setSpaceStartStop(space->currentData().toBool()); });
        connect(tblMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [tblMode, &S](int) { S.setTablePreviewMode(tblMode->currentData().toString()); });
        connect(plMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [plMode, &S](int) { S.setPlayerMode(plMode->currentData().toString()); });
    }

    // ---- Hotkey editor ----
    {
        auto *box = new QGroupBox(tr("Hotkeys (click a shortcut to reassign)"), this);
        auto *lay = new QVBoxLayout(box);
        auto *table = new QTableWidget(S.hotkeyActions().size(), 2, box);
        table->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        const QStringList actions = S.hotkeyActions();
        for (int i = 0; i < actions.size(); ++i) {
            auto *it = new QTableWidgetItem(actions[i]);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            table->setItem(i, 0, it);
            auto *edit = new QKeySequenceEdit(QKeySequence(S.shortcut(actions[i])), box);
            table->setCellWidget(i, 1, edit);
            const QString id = actions[i];
            connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                    [&S, id, edit](const QKeySequence &) {
                        QString seq = edit->keySequence().toString(QKeySequence::NativeText);
                        if (!seq.isEmpty()) S.setShortcut(id, seq);
                    });
        }
        auto *resetBtn = new QPushButton(tr("Reset all to defaults"), box);
        connect(resetBtn, &QPushButton::clicked, this, [&S, table, actions, this] {
            for (const QString &id : actions) S.resetShortcut(id);
            for (int i = 0; i < actions.size(); ++i)
                if (auto *e = qobject_cast<QKeySequenceEdit *>(table->cellWidget(i, 1)))
                    e->setKeySequence(QKeySequence(S.defaultShortcut(actions[i])));
            emit hotkeysChanged();
        });
        lay->addWidget(table);
        lay->addWidget(resetBtn);
        main->addWidget(box);
        connect(&S, &Settings::changed, this, [this] { emit hotkeysChanged(); });
    }

    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    main->addWidget(closeBtn);
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    buildUi();
}
