#pragma once
#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>

class QTableView;
class QTimer;
class FileTableModel;

// Owns everything about the file table's columns: visibility, drag-reorder,
// per-column widths and their persistence (config.json via Settings), plus
// the "Columns" chooser dialog. Sorting stays in MainWindow because it also
// touches selection/scroll behaviour.
class ColumnLayoutController : public QObject {
    Q_OBJECT
public:
    ColumnLayoutController(QTableView *table, FileTableModel *model,
                           QObject *parent = nullptr);

    // MainWindow keeps the selection in sync after a sort; the controller
    // asks for the currently selected file path through this provider.
    void setSelectedPathProvider(std::function<QString()> fn) {
        selectedPathFn_ = std::move(fn);
    }

    // ---- persistence hooks (called by MainWindow on scan/dialog events) ----
    void saveColumnLayout();         // persist column order/visibility to settings
    void applySavedColumnOrder();    // restore visual header order from settings
    void loadColumnWidths();         // read persisted widths from config.json
    void saveColumnWidths();         // persist per-column widths to config.json
    void flushVisibleWidthsToMemory();  // current on-screen widths -> colWidths_
    QTimer *widthSaveTimer() { return widthSaveTimer_; }   // debounced writer

    // ---- header helpers used after reset/sort/move events ----
    void applyTableColumnWidths();   // per-model-column widths after reset/move
    void updateSortIndicator();      // keep the header arrow on the right section
    // Sort the model by a *model* column id and keep the header indicator,
    // widths and the selected row in sync afterwards.
    void sortByModelColumn(int modelCol, Qt::SortOrder order);

    // ---- "Columns" button entry point ----
    // Opens the modal chooser and applies/saves the result. Returns false if
    // the dialog was cancelled or rejected (empty selection).
    bool openDialog(QWidget *parentWindow);

signals:
    // The visible-column set changed (dialog Save): the owner must re-run
    // lazy preview prefetch for the new layout.
    void columnsChanged();

private:
    QTableView *table_ = nullptr;
    FileTableModel *model_ = nullptr;
    QHash<int, int> colWidths_;      // model column id -> persisted width
    QTimer *widthSaveTimer_ = nullptr;   // owned: parented to this QObject
    bool firstWidthLoad_ = true;     // one-time migration of legacy SQLite widths
    std::function<QString()> selectedPathFn_;
};

// Lightweight column chooser used by the "Columns" toolbar button: a popup
// dialog with one checkbox per live column (in the persisted full order) and
// Save/Cancel buttons. Cancel leaves the table untouched; Save applies the
// new visible set and persists it (header order is kept as-is).
class ColumnsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColumnsDialog(const QStringList &keys, const QStringList &labels,
                           const QSet<int> &shown, QWidget *parent = nullptr);
    // Column ids (indices into the label list) checked in the dialog.
    QStringList selectedKeys() const;

#ifdef AUDIOSCOUT_TESTING
    // The "Save" button of the dialog's button box — lets the in-process
    // self-tests (main.cpp, AUDIOSCOUT_COLSAVE) click it like a user would.
    QPushButton *saveButtonForTest() const { return saveButton_; }
#endif

private:
    QStringList keys_;
    QVector<class QCheckBox *> checks_;
    class QPushButton *saveButton_ = nullptr;
};
