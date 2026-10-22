#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QTableView>
#include <QLineEdit>
#include <QPushButton>

#include <QSlider>
#include <QDoubleSpinBox>
#include <QFileSystemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>
#include <QDialog>
#include <QCheckBox>
#include <functional>
class QProcess;
#include "AudioFile.h"
#include "FileTableModel.h"
#include "AudioView.h"
#include "AudioPlayer.h"
#include "UndoStack.h"
#include "Types.h"

class PreviewWorker;

// Result of a background directory scan (used with queued signal delivery).
struct DirScanResult {
    QString dir;
    QVector<FileTableModel::Row> rows;
};
Q_DECLARE_METATYPE(DirScanResult)

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

#ifdef AUDIOSCOUT_TESTING
    // Test hooks (only compiled with -DAUDIOSCOUT_TESTING).
    QTableView *testTable() const { return table_; }
    FileTableModel *testModel() const { return model_; }
    void openColumnsDialogForTest() { openColumnsDialog(); }
#endif

protected:
    void closeEvent(QCloseEvent *) override;

private slots:
    void onDirSelected(const QString &path);
    void onScanFinished(DirScanResult res);
    void onSearchClicked();
    void onRowDoubleClicked();
    void loadCurrentFile();
    void playPause();      // Play/Pause button entry (syncs the label at the end)
    void playPauseInner(); // raw start/pause/resume logic, no final sync
    void stopPlayback();
    void updatePlayButton();
    void prevFile();
    void nextFile();
    void doMute();
    void doNormalize();
    void doDeleteSelection();
    void deleteAllMarkersForFile();
    void renameCurrentFile();
    void undo();
    void redo();
    void showSettings();
    void refreshPreviewsForDir();
    void createMarkerAt(qint64 frame);
    void onMarkerClicked(int idx);
    void openMarkerEditor(int idx);
    void zoomInPlayer();
    void zoomOutPlayer();
    void applyHotkeys();

private:
    void buildUi();
    void applyTableColumnWidths();   // per-model-column widths after reset/move
    void sortByModelColumn(int modelCol, Qt::SortOrder order);  // sort + indicator/selection sync
    void updateSortIndicator();      // keep the header arrow on the right visual section
    void saveColumnLayout();         // persist column order/visibility to settings
    void openColumnsDialog();        // "Columns" button -> chooser dialog (Save/Cancel)
    void applySavedColumnOrder();    // restore visual header order from settings on startup
    void saveColumnWidths();         // persist per-column widths to the SQLite session table
    void loadColumnWidths();         // read persisted widths into memory (once)
    void requestPreview(int row);
    bool editGuarded();          // checks editable format + write perms
    void persistMarkers();
    void applyEditToRange(std::function<void()> op, const QString &type,
                          qint64 off, qint64 len,
                          const QVector<UndoRecord::MarkerDelta> &deltas = {});
    int currentRow() const;      // sorted/view row -> source row mapping handled internally
    void restorePendingSession(); // select+load last file & seek after a scan (spec 10.2)

    // UI
    QFileSystemModel *fsModel_ = nullptr;
    QTreeView *tree_ = nullptr;
    FileTableModel *model_ = nullptr;
    QTableView *table_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QToolButton *columnsBtn_ = nullptr;
    QToolButton *tableModeBtn_ = nullptr;       // Waveform/Spectrogram toggle for table previews
    AudioView *view_ = nullptr;
    QPushButton *playBtn_ = nullptr, *loopBtn_ = nullptr;
    QLabel *activeFileLabel_ = nullptr;         // shows the active file on the action bar
    QToolButton *displayToggleBtn_ = nullptr;   // Waveform/Spectrogram toggle
    QSlider *volumeSlider_ = nullptr;
    QDoubleSpinBox *normLevel_ = nullptr;
    QAction *actRefreshPrev_ = nullptr, *actZoomIn_ = nullptr, *actZoomOut_ = nullptr;
    QAction *actSettings_ = nullptr;

    // State
    QString currentDir_;
    QString currentPath_;
    AudioData audio_;
    QVector<Marker> markers_;
    UndoStack undoStack_;
    AudioPlayer player_;
    // Direct ffmpeg fallback: when neither Qt Multimedia nor the in-process
    // pipe sinks can produce sound, we spawn "ffmpeg -re -i tmpfile" so that
    // playback always works. playProc_ owns the temp file path.
    QProcess *playProc_ = nullptr;
    QString playTmpPath_;
    // The in-process pipe sink currently owned by AudioPlayer (aplay/ffplay).
    // Kept here so the volume slider can respawn aplay with a new -v value.
    class ProcessSink *pipeSink_ = nullptr;   // defined in MainWindow.cpp
    QAudioFormat pipeFmt_;
    // Current fallback range in frames (for pause/resume and loop restart).
    qint64 fbStartFrame_ = 0, fbEndFrame_ = 0;
    // Frame where the last Play session started (Qt path). Stop keeps the
    // playhead in place; pressing Play again resumes from this point.
    qint64 lastPlayStartFrame_ = -1;
    bool fbLooping_ = false;
    bool wasPlaying_ = false;           // fallback session active (vs. stopped/paused)
    bool fbPaused_ = false;             // paused fallback session kept for resume
    bool seekingPlayback_ = false;      // re-entrancy guard: setPlayheadFrame →
                                        // cursorMoved → seekPlaybackTo must not loop
    // Session restore (spec 10.2): last file/position are applied after the
    // directory scan completes, since rows only exist then.
    QString pendingSessionFile_;
    QHash<int, int> colWidths_;   // model column id -> persisted width (SQLite session)
    QTimer colWidthSaveTimer_;    // debounce for saveColumnWidths()
    double  pendingSessionPosSec_ = 0.0;
    qint64 fbElapsedMs_ = 0;            // wall-clock ms the fallback proc ran
    QElapsedTimer fbClock_;             // timing for fbElapsedMs_
    qint64 fbFromFrame_ = 0;            // frame playback started from (this proc)
    QTimer *fbPlayheadTimer_ = nullptr; // advances the playhead during fallback
    void startFfmpegFallback(qint64 startFrame, qint64 endFrame, bool loop);
    void stopFfmpegFallback();
    void resumeFfmpegFallback();
    // Relaunch the running ffmpeg fallback at the current playhead with the
    // newly stored volume (the external process reads gain only at startup).
    void restartFfmpegFallbackWithVolume();
    // Seek during playback: moves the playhead and continues from the new
    // position in whichever output path is active (Qt sink, pipe fallback or
    // external ffmpeg process). No-op when nothing is playing.
    void seekPlaybackTo(qint64 frame);
    // Refresh the active-file indicators: the label on the action panel and
    // the window title ("AudioScout - <file name>").
    void updateActiveFileDisplay();
    PreviewWorker *previewWorker_ = nullptr;
};

// Lightweight column chooser used by the "Columns" toolbar button: a popup
// dialog with one checkbox per live column (in the persisted full order) and
// Save/Cancel buttons. Cancel leaves the table untouched; Save applies the
// new visible set and persists it (header order is kept as-is).
class ColumnsDialog : public QDialog {
public:
    explicit ColumnsDialog(const QStringList &keys, const QStringList &labels,
                           const QSet<int> &shown, QWidget *parent = nullptr);
    // Column ids (indices into the label list) checked in the dialog.
    QStringList selectedKeys() const;

private:
    QStringList keys_;
    QVector<QCheckBox *> checks_;
};
