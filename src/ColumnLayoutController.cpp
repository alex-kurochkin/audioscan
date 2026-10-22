#include "ColumnLayoutController.h"

#include "Database.h"
#include "FileTableModel.h"
#include "Settings.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

// Compute the persisted full order of live columns (visible + hidden) and the
// currently shown set, honouring legacy settings keys. Shared by the Columns
// dialog so newly-shown columns land where drag-reorder left them.
static void currentColumnLayout(FileTableModel *model, QVector<int> &fullOrder,
                                QSet<int> &shown) {
    QStringList fullKeys = Settings::instance().columnOrder();
    if (fullKeys.isEmpty()) fullKeys = Settings::instance().visibleColumns();
    for (const QString &key : fullKeys) {
        int col = FileTableModel::columnFromKey(key);
        // The Folder column is retired — drop it even if an old settings
        // file still lists it.
        if (col >= 0 && FileTableModel::isLiveColumn(col) && !fullOrder.contains(col))
            fullOrder << col;
    }
    for (int c = 0; c <= int(FileTableModel::ColModified); ++c)
        if (!fullOrder.contains(c)) fullOrder << c;

    // Visible set: intersection with the visible-columns key, so an upgrade
    // from older configs keeps exactly the previously shown columns.
    const QStringList visKeys = Settings::instance().visibleColumns();
    if (!visKeys.isEmpty()) {
        for (const QString &key : visKeys) {
            int col = FileTableModel::columnFromKey(key);
            if (col >= 0 && FileTableModel::isLiveColumn(col)) shown.insert(col);
        }
    } else {
        for (int c : fullOrder) shown.insert(c);
    }
    Q_UNUSED(model);
}

ColumnLayoutController::ColumnLayoutController(QTableView *table,
                                               FileTableModel *model,
                                               QObject *parent)
    : QObject(parent), table_(table), model_(model) {
    // Debounced writer for user-resized column widths (config.json).
    widthSaveTimer_ = new QTimer(this);
    widthSaveTimer_->setSingleShot(true);
    connect(widthSaveTimer_, &QTimer::timeout, this, [this] {
        saveColumnWidths();
        // Keep the in-memory copy in sync so a later re-scan restores them.
        flushVisibleWidthsToMemory();
    });
}

void ColumnLayoutController::flushVisibleWidthsToMemory() {
    if (!table_ || !model_) return;
    auto *hdr = table_->horizontalHeader();
    const auto &visCols = model_->visibleColumns();
    for (int log = 0; log < visCols.size(); ++log)
        if (!hdr->isSectionHidden(log))
            colWidths_.insert(visCols[log], table_->columnWidth(log));
}

// Sort the model by a *model* column id and keep the header indicator,
// widths and the selected row in sync afterwards.
void ColumnLayoutController::sortByModelColumn(int modelCol, Qt::SortOrder order) {
    // Remember the selection by file path — sorting resets the model, so
    // row indices are not stable across it.
    QString selPath = selectedPathFn_ ? selectedPathFn_() : QString();

    model_->sortByModelColumn(modelCol, order);
    updateSortIndicator();
    applyTableColumnWidths();

    if (!selPath.isEmpty()) {
        const auto &rows = model_->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].path == selPath) {
                table_->selectRow(i);
                break;
            }
        }
    }
}

// Map the current sort column back to its visual header section so the
// indicator stays on the right column after drag-reordering.
void ColumnLayoutController::updateSortIndicator() {
    if (!table_ || !model_) return;
    const int vis = model_->visibleColumns().indexOf(model_->sortColumn());
    if (vis < 0) { table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder); return; }
    table_->horizontalHeader()->setSortIndicator(
        table_->horizontalHeader()->visualIndex(vis), model_->sortOrder());
}

// Set a comfortable width for every visible section, mapping the visual
// section back to its model column via the header's logical index.
void ColumnLayoutController::applyTableColumnWidths() {
    static const int kColW[FileTableModel::ColCount] =
        {220, 350, 70, 90, 90, 80, 90, 140, 130, 130};
    if (!table_ || !model_) return;
    for (int vis = 0; vis < model_->columnCount(); ++vis) {
        int logical = table_->horizontalHeader()->logicalIndex(vis);
        if (logical < 0) continue;
        int modelCol = model_->visibleColumns().value(logical, -1);
        if (modelCol >= 0 && modelCol < int(FileTableModel::ColCount)) {
            // User-resized width wins over the built-in default (spec: sizes
            // persist across restarts; stored in config.json).
            const int w = colWidths_.value(modelCol, kColW[modelCol]);
            if (table_->columnWidth(logical) != w)
                table_->setColumnWidth(logical, w);
        }
    }
}

// Persist the current visual order of all live columns (visible ones first,
// in header order; hidden ones appended in their last known relative order).
void ColumnLayoutController::saveColumnLayout() {
    if (!table_ || !model_) return;
    auto *hdr = table_->horizontalHeader();
    const auto &visCols = model_->visibleColumns();
    QStringList visibleOrder;
    for (int log = 0; log < visCols.size(); ++log) {
        const int v = hdr->visualIndex(log);
        if (v < 0) continue;
        visibleOrder << QStringLiteral("%1:%2").arg(v).arg(FileTableModel::columnKey(visCols.value(log)));
    }
    std::sort(visibleOrder.begin(), visibleOrder.end());
    QStringList shownKeys;
    for (const QString &s : visibleOrder) shownKeys << s.section(':', 1);

    // Keep hidden columns in their previously saved relative order.
    QStringList prevFull = Settings::instance().columnOrder();
    if (prevFull.isEmpty()) prevFull = Settings::instance().visibleColumns();
    QStringList full = shownKeys;
    for (const QString &k : prevFull)
        if (!full.contains(k)) full << k;
    Settings::instance().setColumnOrder(full);
    Settings::instance().setVisibleColumns(shownKeys);
}

// Re-apply the persisted visual order of the header sections after a model
// reset (directory scan). moveSection() is idempotent: if the header already
// matches the saved order, no sectionMoved signal fires and nothing changes.
void ColumnLayoutController::applySavedColumnOrder() {
    if (!table_ || !model_) return;
    auto *hdr = table_->horizontalHeader();
    QStringList fullKeys = Settings::instance().columnOrder();
    // Fall back to the visible-columns key for legacy configs that predate
    // the separate "full order" entry.
    if (fullKeys.isEmpty()) fullKeys = Settings::instance().visibleColumns();
    if (fullKeys.isEmpty()) return;
    const auto &visCols = model_->visibleColumns();
    QVector<int> targetVis;   // logical indices in the desired visual order
    for (const QString &key : fullKeys) {
        const int col = FileTableModel::columnFromKey(key);
        if (col < 0) continue;
        const int log = visCols.indexOf(col);
        if (log >= 0 && !targetVis.contains(log)) targetVis << log;
    }
    for (int log = 0; log < visCols.size(); ++log)
        if (!targetVis.contains(log)) targetVis << log;   // unseen columns last
    for (int target = 0; target < targetVis.size(); ++target) {
        const int cur = hdr->visualIndex(targetVis[target]);
        if (cur != target) hdr->moveSection(cur, target);
    }
}

// Read the per-column widths from config.json (all column settings live
// there). Reloaded on every call (cheap) so widths saved by the Columns
// dialog mid-run are picked up immediately. Widths previously stored in the
// SQLite session table (older builds) are migrated once on the first load.
void ColumnLayoutController::loadColumnWidths() {
    const QMap<QString, int> map = Settings::instance().columnWidths();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const int col = FileTableModel::columnFromKey(it.key());
        if (col >= 0 && col < int(FileTableModel::ColCount) && it.value() > 0)
            colWidths_.insert(col, it.value());
    }
    if (firstWidthLoad_) {
        firstWidthLoad_ = false;
        const QString legacy = Database::instance().sessionValue(QStringLiteral("colWidths"));
        if (!legacy.isEmpty()) {
            const QJsonObject obj = QJsonDocument::fromJson(legacy.toUtf8()).object();
            QMap<QString, int> merged = Settings::instance().columnWidths();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                const int col = it.key().toInt();
                const int w = it.value().toInt();
                if (w > 0 && col >= 0 && col < int(FileTableModel::ColCount))
                    merged.insert(FileTableModel::columnKey(col), w);
            }
            Settings::instance().setColumnWidths(merged);
            Database::instance().setSessionValue(QStringLiteral("colWidths"), QString());
        }
    }
}

// Persist the current width of every visible column into config.json so
// user-resized columns keep their size across restarts.
void ColumnLayoutController::saveColumnWidths() {
    if (!table_ || !model_) return;
    auto *hdr = table_->horizontalHeader();
    const auto &visCols = model_->visibleColumns();
    QMap<QString, int> map = Settings::instance().columnWidths();
    for (int log = 0; log < visCols.size(); ++log) {
        if (hdr->isSectionHidden(log)) continue;
        map.insert(FileTableModel::columnKey(visCols[log]),
                   table_->columnWidth(log));
    }
    Settings::instance().setColumnWidths(map);
}

void ColumnLayoutController::openDialog(QWidget *parentWindow) {
    QVector<int> fullOrder;
    QSet<int> shown;
    currentColumnLayout(model_, fullOrder, shown);

    QStringList keys, labels;
    for (int c : fullOrder) {
        keys << FileTableModel::columnKey(c);
        labels << model_->columnName(c);
    }

    ColumnsDialog dlg(keys, labels, shown, parentWindow);
    dlg.setWindowTitle(dlg.tr("Columns"));
    if (dlg.exec() != QDialog::Accepted) return;   // Cancel: nothing changes

    const QStringList selKeys = dlg.selectedKeys();
    if (selKeys.isEmpty()) {
        QMessageBox::warning(parentWindow, dlg.tr("Columns"),
                             dlg.tr("At least one column must be visible."));
        return;
    }
    // Flush the currently displayed widths into the in-memory map first, so
    // columns that are about to be re-shown come back with their last size
    // instead of the built-in default.
    widthSaveTimer_->stop();
    flushVisibleWidthsToMemory();

    // Keep the persisted relative order; just filter to the chosen set.
    QVector<int> vis;
    QStringList visKeys;
    for (int i = 0; i < fullOrder.size(); ++i) {
        if (selKeys.contains(keys.at(i))) {
            vis << fullOrder[i];
            visKeys << keys.at(i);
        }
    }
    model_->updateVisibleColumns(vis);

    // updateVisibleColumns() resets the model -> the header falls back to the
    // default order; re-apply the persisted drag-reorder right away so the
    // visible arrangement matches what was saved on the previous run.
    applySavedColumnOrder();

    // Persist immediately (Save click): visible set + full order. The next
    // start restores both from config.json via the persisted keys.
    Settings::instance().setVisibleColumns(visKeys);
    Settings::instance().setColumnOrder(keys);
    loadColumnWidths();                       // pick up widths saved mid-run
    saveColumnWidths();                       // widths -> config.json
    QTimer::singleShot(0, this, [this] {      // after the model reset settles
        applySavedColumnOrder();              // keep drag-reorder of header
        applyTableColumnWidths();             // restore per-column sizes
        updateSortIndicator();
    });
    emit columnsChanged();
}

ColumnsDialog::ColumnsDialog(const QStringList &keys, const QStringList &labels,
                             const QSet<int> &shown, QWidget *parent)
    : QDialog(parent), keys_(keys) {
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("Show columns:")));
    for (int i = 0; i < labels.size(); ++i) {
        auto *cb = new QCheckBox(labels[i], this);
        cb->setChecked(shown.contains(i));
        checks_.append(cb);
        lay->addWidget(cb);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addStretch();
    lay->addWidget(bb);
    setWindowTitle(tr("Columns"));
}

QStringList ColumnsDialog::selectedKeys() const {
    QStringList out;
    for (int i = 0; i < checks_.size(); ++i)
        if (checks_[i]->isChecked()) out << keys_.at(i);
    return out;
}
