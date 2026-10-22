#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QTableView>
#include <QLineEdit>
#include <QPushButton>

#include <QSlider>
#include <QDoubleSpinBox>
#include <QFileSystemModel>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>
#include <functional>

#include "AudioFile.h"
#include "FileTableModel.h"
#include "AudioView.h"
#include "AudioPlayer.h"
#include "UndoStack.h"
#include "Types.h"

#include "DirScanner.h"               // DirScanResult travels in queued signals
#include "PlayerController.h"         // MainWindow implements PlayerUi
#include "ColumnLayoutController.h"

class PreviewWorker;
class FfmpegFallbackController;

// Main application window: owns the widgets and the document state (audio
// buffer, markers, undo stack) and delegates all subsystem logic to dedicated
// controllers:
//   PlayerController          — play/pause/stop/seek/loop decisions
//   FfmpegFallbackController  — the external ffmpeg playback process
//   ColumnLayoutController    — table column visibility/order/widths/dialog
//   DirScanner                — background directory scans
//   PreviewWorker             — async waveform/spectrogram thumbnails
// Everything left in here is UI wiring, file loading, editing and markers.
class MainWindow : public QMainWindow, public PlayerUi {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

#ifdef AUDIOSCOUT_TESTING
    // Test hooks (only compiled with -DAUDIOSCOUT_TESTING).
    QTableView *testTable() const { return table_; }
    FileTableModel *testModel() const { return model_; }
    void openColumnsDialogForTest() { cols_->openDialog(this); }
#endif

protected:
    void closeEvent(QCloseEvent *) override;

private slots:
    void onDirSelected(const QString &path);
    void onScanFinished(DirScanResult res);
    void onSearchClicked();
    void onRowDoubleClicked();
    void loadCurrentFile();
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
    void applyHotkeys();

private:
    // ---- PlayerUi: the playback engine talks back through these ----
    void setPlayhead(qint64 frame, bool programmatic) override;
    qint64 playhead() const override;
    void showStatus(const QString &msg, int msec) override;
    void rangeChanged(qint64 startFrame, qint64 endFrame) override;

    void buildUi();
    void wirePlayerSignals();     // player/fallback -> UI connections
    void requestPreview(int row);
    void applyPreviewMode(const QString &mode);   // switch previews to one mode atomically
    bool editGuarded();          // checks editable format + write perms
    void persistMarkers();
    void applyEditToRange(std::function<void()> op, const QString &type,
                          qint64 off, qint64 len,
                          const QVector<UndoRecord::MarkerDelta> &deltas = {});
    int currentRow() const;      // sorted/view row -> source row mapping handled internally
    int previewGeneration_ = 0;  // bumped on every mode switch/refresh: stale
                                 // worker results of an older generation are dropped,
                                 // so the table never mixes wave and spectrogram rows
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
    QAction *actRefreshPrev_ = nullptr;
    QAction *actSettings_ = nullptr;

    // State
    QString currentDir_;
    QString currentPath_;
    AudioData audio_;
    QVector<Marker> markers_;
    UndoStack undoStack_;
    AudioPlayer player_;
    bool seekingPlayback_ = false;      // re-entrancy guard: setPlayheadFrame →
                                        // cursorMoved → seekPlaybackTo must not loop
    // Session restore (spec 10.2): last file/position are applied after the
    // directory scan completes, since rows only exist then.
    QString pendingSessionFile_;
    double  pendingSessionPosSec_ = 0.0;

    // Controllers extracted from this god-class (see the class comment).
    DirScanner *scanner_ = nullptr;
    FfmpegFallbackController *fb_ = nullptr;
    PlayerController *pc_ = nullptr;
    ColumnLayoutController *cols_ = nullptr;

    void seekPlaybackTo(qint64 frame);  // forwards to pc_ (click-to-seek handler)
    // Refresh the active-file indicators: the label on the action panel and
    // the window title ("AudioScout - <file name>").
    void updateActiveFileDisplay();
    PreviewWorker *previewWorker_ = nullptr;
};
