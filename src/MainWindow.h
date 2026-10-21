#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QTableView>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QFileSystemModel>
#include <QElapsedTimer>
#include <QToolBar>
#include <QToolButton>
#include <functional>
class QProcess;
#include "AudioFile.h"
#include "FileDialogModel.h"
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

protected:
    void closeEvent(QCloseEvent *) override;

private slots:
    void onDirSelected(const QString &path);
    void onScanFinished(DirScanResult res);
    void onSearchClicked();
    void onRowDoubleClicked();
    void loadCurrentFile();
    void playPause();
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
    void applyHotkeys();

private:
    void buildUi();
    void applyTableColumnWidths();   // per-model-column widths after reset/move
    void requestPreview(int row);
    bool editGuarded();          // checks editable format + write perms
    void persistMarkers();
    void applyEditToRange(std::function<void()> op, const QString &type,
                          qint64 off, qint64 len,
                          const QVector<UndoRecord::MarkerDelta> &deltas = {});
    void rebuildColumnMenu();
    int currentRow() const;      // sorted/view row -> source row mapping handled internally

    // UI
    QFileSystemModel *fsModel_ = nullptr;
    QTreeView *tree_ = nullptr;
    FileTableModel *model_ = nullptr;
    QTableView *table_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QToolButton *columnsBtn_ = nullptr;
    QComboBox *tableModeCombo_ = nullptr;
    AudioView *view_ = nullptr;
    QPushButton *playBtn_ = nullptr, *loopBtn_ = nullptr;
    QSlider *volumeSlider_ = nullptr;
    QComboBox *playerModeCombo_ = nullptr;
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
    // Current fallback range in frames (for pause/resume and loop restart).
    qint64 fbStartFrame_ = 0, fbEndFrame_ = 0;
    bool fbLooping_ = false;
    bool wasPlaying_ = false;           // fallback session active (vs. stopped/paused)
    qint64 fbElapsedMs_ = 0;            // wall-clock ms the fallback proc ran
    QElapsedTimer fbClock_;             // timing for fbElapsedMs_
    qint64 fbFromFrame_ = 0;            // frame playback started from (this proc)
    QTimer *fbPlayheadTimer_ = nullptr; // advances the playhead during fallback
    void startFfmpegFallback(qint64 startFrame, qint64 endFrame, bool loop);
    void stopFfmpegFallback();
    void resumeFfmpegFallback();
    PreviewWorker *previewWorker_ = nullptr;
};
