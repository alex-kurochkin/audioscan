#include "MainWindow.h"
#include "Settings.h"
#include "Database.h"
#include "Renderer.h"
#include "MarkerDialog.h"
#include "SettingsDialog.h"
#include "PreviewWorker.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QTextStream>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QShortcut>
#include <QInputDialog>
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include "DirScanner.h"
#include "FfmpegFallbackController.h"
#include "PlayerController.h"
#include "ProcessSink.h"
#include "AudioDebug.h"

#include <QStandardPaths>

#include <QVBoxLayout>
#include <QMediaDevices>
#include <QFileInfo>
#include <algorithm>

// Directory tree model: only directories are listed (spec 2.3), and their
// names are always visible even before the async filesystem watcher has
// populated them (QFileSystemModel returns empty DisplayRole data for
// not-yet-indexed entries — we fall back to QFileInfo).
class DirTreeModel : public QFileSystemModel {
public:
    explicit DirTreeModel(QObject *parent = nullptr) : QFileSystemModel(parent) {
        setNameFilterDisables(false);
        setFilter(QDir::Dirs | QDir::Drives | QDir::NoDotAndDotDot);
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid()) return {};
        if (role == Qt::DecorationRole && index.column() == 0)
            return QFileSystemModel::data(index, role);
        if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
        QString name = QFileSystemModel::data(index, role).toString();
        if (name.isEmpty())
            name = QFileInfo(filePath(index)).fileName();
        if (name.isEmpty() && !filePath(index).isEmpty())
            name = filePath(index);   // root
        return name;
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        if (!index.isValid()) return Qt::NoItemFlags;
        return QFileSystemModel::flags(index) | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }
    // The tree shows a single meaningful column ("Folders"). We collapse the
    // default 5-column QFileSystemModel (Name|Size|Type|Date Modified|Folder)
    // down to one, which also removes the "Folder" column that has no meaning
    // in a directory-only view.
    int columnCount(const QModelIndex & /*parent*/ = QModelIndex()) const override {
        return 1;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0)
            return tr("Folders");
        return QFileSystemModel::headerData(section, orientation, role);
    }
};

// ---------------- helpers ----------------

// ---------------- construction ----------------

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    buildUi();
    applyHotkeys();
    connect(&Settings::instance(), &Settings::changed, this, &MainWindow::applyHotkeys);

    // restore last session state (spec 10.2): directory is applied right away,
    // the selected file and playhead position are applied once the scan of
    // that directory has filled the table (see restorePendingSession()).
    QString dir, file; double pos = 0.0;
    Database::instance().getSessionState(dir, file, pos);
    if (!dir.isEmpty() && QDir(dir).exists()) {
        // Expand the tree down to the restored directory so its node is
        // visible and selected in the left panel (QDirModel resolves each
        // path component lazily, index() walks/creates the hierarchy).
        QModelIndex idx = fsModel_->index(dir);
        while (idx.isValid()) {
            tree_->setExpanded(idx, true);
            idx = idx.parent();
        }
        tree_->setCurrentIndex(fsModel_->index(dir));
        tree_->scrollTo(fsModel_->index(dir));
        if (!file.isEmpty() && QFileInfo(file).exists() &&
            QFileInfo(file).absolutePath() == QDir(dir).absolutePath()) {
            pendingSessionFile_ = file;
            pendingSessionPosSec_ = pos;
        }
        // Auto-load the files of the restored directory into the table.
        onDirSelected(dir);
    } else {
        onDirSelected(QDir::homePath());
        tree_->setCurrentIndex(fsModel_->index(QDir::homePath()));
    }
}

// Apply the pending file selection/position after a directory scan finished.
void MainWindow::restorePendingSession() {
    if (pendingSessionFile_.isEmpty()) return;
    int row = -1;
    const auto &rows = model_->rows();
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].path == pendingSessionFile_) { row = i; break; }
    }
    if (row < 0 || model_->rows()[row].broken) {   // file gone or broken: drop it
        pendingSessionFile_.clear();
        return;
    }
    const QString file = pendingSessionFile_;
    const double posSec = pendingSessionPosSec_;
    pendingSessionFile_.clear();                   // consume before loading
    table_->selectRow(row);
    table_->scrollTo(model_->index(row, 0));
    loadCurrentFile();
    if (audio_.valid && audio_.sampleRate > 0 && posSec > 0.0) {
        const qint64 frame = std::clamp<qint64>(
            qint64(posSec * audio_.sampleRate), 0, audio_.totalFrames);
        setPlayhead(frame, true);              // Play resumes where the session left off
        pc_->setLastPlayStartFrame(frame);
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *e) {
    player_.stop();
    fb_->stop();
    // Persist the final header order (drag-reorder done right before closing
    // is saved on every sectionMoved, but this guarantees the last state).
    cols_->saveColumnLayout();
    cols_->widthSaveTimer()->stop();     // flush any pending width save below
    cols_->saveColumnWidths();
    // Only persist the session we actually own: if startup restored a stale
    // lastFile that was never loaded (user didn't touch anything), keep it
    // untouched instead of clobbering good state with an empty one.
    QString file = currentPath_;
    double pos = audio_.sampleRate ? double(view_->cursorFrame()) / audio_.sampleRate : 0.0;
    if (file.isEmpty() && !pendingSessionFile_.isEmpty()) {
        file = pendingSessionFile_;
        pos = pendingSessionPosSec_;
    }
    Database::instance().setSessionState(currentDir_, file, pos);
    e->accept();
}

// ---------------- UI ----------------

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("AudioScout"));
    resize(1280, 860);

    // Background directory scanner (extracted from this class): scans never
    // block the UI thread; results arrive through finished().
    scanner_ = new DirScanner(this);
    connect(scanner_, &DirScanner::finished, this, &MainWindow::onScanFinished);

    fsModel_ = new DirTreeModel(this);
    fsModel_->setRootPath(QDir::rootPath());
    fsModel_->setNameFilterDisables(false);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(4, 4, 4, 4);
    setCentralWidget(central);

    // ---- top toolbar ----
    auto *tb = new QToolBar(this);
    tb->setMovable(false);
    addToolBar(Qt::TopToolBarArea, tb);

    searchEdit_ = new QLineEdit(tb);
    searchEdit_->setPlaceholderText(tr("Search file by name..."));
    searchEdit_->setClearButtonEnabled(true);
    tb->addWidget(searchEdit_);
    auto *searchBtn = new QPushButton(tr("Search"), tb);
    tb->addWidget(searchBtn);
    connect(searchBtn, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSearchClicked);

    columnsBtn_ = new QToolButton(tb);
    columnsBtn_->setText(tr("Columns"));
    columnsBtn_->setToolTip(tr("Choose which columns to display"));
    tb->addWidget(columnsBtn_);

    // Single toggle button instead of a dropdown: shows the mode it will switch TO.
    tableModeBtn_ = new QToolButton(tb);
    const bool tableSpec = Settings::instance().tablePreviewMode() == QLatin1String("spectrogram");
    tableModeBtn_->setText(tableSpec ? tr("Spectrogram") : tr("Waveform"));
    tableModeBtn_->setToolTip(tr("Click to switch table previews between Waveform and Spectrogram"));
    tableModeBtn_->setAutoRaise(false);
    tb->addWidget(new QLabel(tr(" Table: "), tb));
    tb->addWidget(tableModeBtn_);

    actRefreshPrev_ = new QAction(tr("Refresh previews"), tb);
    tb->addAction(actRefreshPrev_);

    actSettings_ = tb->addAction(tr("Settings"));

    connect(actRefreshPrev_, &QAction::triggered, this, &MainWindow::refreshPreviewsForDir);
    connect(actSettings_, &QAction::triggered, this, &MainWindow::showSettings);
    connect(tableModeBtn_, &QToolButton::clicked, this, [this] {
        const bool toSpectrogram =
            Settings::instance().tablePreviewMode() != QLatin1String("spectrogram");
        tableModeBtn_->setText(toSpectrogram ? tr("Spectrogram") : tr("Waveform"));
        // Atomic switch: in-flight previews of the old mode are discarded and
        // the whole visible batch is re-rendered in the new mode.
        applyPreviewMode(toSpectrogram ? QStringLiteral("spectrogram")
                                       : QStringLiteral("wave"));
    });

    // ---- splitter: tree | table ----
    auto *mainSplit = new QSplitter(Qt::Vertical, central);
    outer->addWidget(mainSplit, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, mainSplit);

    tree_ = new QTreeView(splitter);
    tree_->setModel(fsModel_);
    tree_->setRootIndex(fsModel_->index(QDir::rootPath()));
    // Directory tree shows a single column with a visible title ("Folders").
    // Width is user-resizable; the "Folder" column of QFileSystemModel is
    // removed at the model level (DirTreeModel::columnCount == 1).
    {
        auto *hdr = tree_->header();
        hdr->setVisible(true);
        hdr->setSectionResizeMode(0, QHeaderView::Interactive);
        hdr->setStretchLastSection(true);
        hdr->resizeSection(0, 260);
    }
    splitter->addWidget(tree_);

    auto *rightPanel = new QWidget(splitter);
    auto *rightLay = new QVBoxLayout(rightPanel);
    rightLay->setContentsMargins(0, 0, 0, 0);

    model_ = new FileTableModel(rightPanel);
    table_ = new QTableView(rightPanel);
    table_->setModel(model_);
    // Sorting is applied manually via the header-click handler below (the
    // model has no stable sort proxy — setSortingEnabled(true) would make
    // Qt re-sort with a visual/logical column index and corrupt the order).
    table_->setSortingEnabled(false);
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSectionsMovable(true);   // drag to reorder (spec 2.4)
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setIconSize(QSize(340, 20));
    table_->setColumnWidth(0, 220);
    table_->horizontalHeader()->setStretchLastSection(true);
    // Give every column enough width so its content is actually visible.
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    static const int kColW[FileTableModel::ColCount] =
        {220, 350, 70, 90, 90, 80, 90, 140, 130, 130};
    for (int c = 0; c < FileTableModel::ColCount; ++c)
        table_->setColumnWidth(c, kColW[c]);
    rightLay->addWidget(table_, 1);
    splitter->addWidget(rightPanel);
    splitter->setSizes({260, 900});

    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &i) {
        onDirSelected(fsModel_->filePath(i));
    });
    // Expanding a tree node must NOT rescan the currently loaded directory:
    // that would clobber the table right after the startup session restore.
    // Only an explicit click (or double-click) switches the loaded directory.
    // Single click only highlights the row; double click loads the file
    // into the player (spec: file load via double click).
    connect(table_, &QTableView::doubleClicked,
            this, &MainWindow::onRowDoubleClicked);

    // Sorting is handled manually by the model (FileTableModel): QTableView's
    // built-in sort-on-click would hand us a *visual/logical* header section,
    // which mismatches the model column ids once columns are reordered or
    // hidden. Track header clicks ourselves and translate them to model cols.
    table_->setSortingEnabled(false);
    table_->horizontalHeader()->setSortIndicatorShown(true);

    // Column layout controller: owns visibility/order/width persistence and
    // the "Columns" dialog. It asks for the selected file path so a sort can
    // keep the selection in sync.
    cols_ = new ColumnLayoutController(table_, model_, this);
    cols_->setSelectedPathProvider([this] {
        const int row = currentRow();
        return (row >= 0 && row < model_->rows().size()) ? model_->rows()[row].path
                                                         : QString();
    });
    connect(columnsBtn_, &QToolButton::clicked, this, [this] {
        cols_->openDialog(this);
    });
    connect(cols_, &ColumnLayoutController::columnsChanged, this, [this] {
        // Visible set changed: re-run the lazy preview prefetch for the
        // new layout (the generation counter drops stale in-flight results).
        refreshPreviewsForDir();
    });

    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, [this](int visual) {
        const int logical = table_->horizontalHeader()->logicalIndex(visual);
        if (logical < 0) return;
        const int modelCol = model_->visibleColumns().value(logical, -1);
        if (modelCol == FileTableModel::ColPreview ||
            modelCol == FileTableModel::ColComment) {
            // Sorting forbidden per spec 2.4 — keep the current indicator.
            cols_->updateSortIndicator();
            return;
        }
        Qt::SortOrder order = Qt::AscendingOrder;
        if (modelCol == model_->sortColumn())
            order = (model_->sortOrder() == Qt::AscendingOrder)
                        ? Qt::DescendingOrder : Qt::AscendingOrder;
        cols_->sortByModelColumn(modelCol, order);
    });
    // After a header drag-reorder, persist the new visual order of *all*
    // columns (visible + hidden) so it survives restarts.
    connect(table_->horizontalHeader(), &QHeaderView::sectionMoved,
            this, [this] { cols_->saveColumnLayout(); cols_->applyTableColumnWidths();
                           cols_->updateSortIndicator(); });
    // Persist user-resized column widths (debounced) into config.json.
    connect(table_->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int /*logical*/, int /*oldSize*/, int newSize) {
                if (newSize <= 0 || !model_) return;   // hide/reset noise
                const auto &visCols = model_->visibleColumns();
                if (visCols.isEmpty()) return;         // no rows yet: nothing to store
                cols_->widthSaveTimer()->start(300);
            });
    connect(model_, &QAbstractItemModel::modelReset,
            this, [this] { QTimer::singleShot(0, this, [this] { cols_->applyTableColumnWidths(); }); });
    connect(model_, &FileTableModel::previewRequested, this, &MainWindow::requestPreview);

    // ---- action panel (active file) — inside the resizable lower pane ----
    auto *lowerPane = new QWidget(mainSplit);
    auto *lowerLay = new QVBoxLayout(lowerPane);
    lowerLay->setContentsMargins(0, 0, 0, 0);
    lowerLay->setSpacing(2);

    auto *actionsBar = new QWidget(lowerPane);
    auto *ab = new QHBoxLayout(actionsBar);
    ab->setContentsMargins(0, 2, 0, 2);
    auto *muteBtn = new QPushButton(tr("Mute"), actionsBar);
    auto *normBtn = new QPushButton(tr("Normalize"), actionsBar);
    normLevel_ = new QDoubleSpinBox(actionsBar);
    normLevel_->setRange(-60.0, 0.0);
    normLevel_->setValue(-1.0);
    normLevel_->setSuffix(QStringLiteral(" dBFS"));
    auto *delSelBtn = new QPushButton(tr("Delete selection"), actionsBar);
    auto *delAllMarksBtn = new QPushButton(tr("Delete all markers"), actionsBar);
    auto *renameBtn = new QPushButton(tr("Rename..."), actionsBar);
    ab->addWidget(muteBtn);
    ab->addWidget(normBtn);
    ab->addWidget(normLevel_);
    ab->addWidget(delSelBtn);
    ab->addWidget(delAllMarksBtn);
    ab->addWidget(renameBtn);
    // Name of the currently loaded (active) file, right after the Rename button.
    activeFileLabel_ = new QLabel(actionsBar);
    activeFileLabel_->setTextFormat(Qt::PlainText);
    QFont afFont = activeFileLabel_->font();
    afFont.setBold(true);
    activeFileLabel_->setFont(afFont);
    ab->addSpacing(12);
    ab->addWidget(activeFileLabel_, 1);
    ab->addStretch(2);
    lowerLay->addWidget(actionsBar);

    connect(muteBtn, &QPushButton::clicked, this, &MainWindow::doMute);
    connect(normBtn, &QPushButton::clicked, this, &MainWindow::doNormalize);
    connect(delSelBtn, &QPushButton::clicked, this, &MainWindow::doDeleteSelection);
    connect(delAllMarksBtn, &QPushButton::clicked, this, &MainWindow::deleteAllMarkersForFile);
    connect(renameBtn, &QPushButton::clicked, this, &MainWindow::renameCurrentFile);

    // ---- player bar ----
    auto *playerBar = new QWidget(lowerPane);
    auto *pb = new QHBoxLayout(playerBar);
    pb->setContentsMargins(0, 2, 0, 2);
    auto *prevBtn = new QPushButton(tr("|<"), playerBar);
    playBtn_ = new QPushButton(tr("Play"), playerBar);
    auto *stopBtn = new QPushButton(tr("Stop"), playerBar);
    auto *nextBtn = new QPushButton(tr(">|"), playerBar);
    loopBtn_ = new QPushButton(tr("Loop"), playerBar);
    loopBtn_->setCheckable(true);
    // The highlight must track *actual* loop activity (armed AND a session is
    // running), not the checked state: an armed-but-idle Loop stays unlit.
    {
        QPalette lit = loopBtn_->palette();
        const QColor accent(70, 130, 220);
        lit.setColor(QPalette::Button, accent);
        lit.setColor(QPalette::ButtonText, Qt::white);
        lit.setColor(QPalette::Highlight, accent.darker(120));
        loopBtn_->setProperty("litPalette", QVariant::fromValue(lit));
        loopBtn_->setProperty("basePalette", QVariant::fromValue(loopBtn_->palette()));
    }
    volumeSlider_ = new QSlider(Qt::Horizontal, playerBar);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(int(Settings::instance().volume() * 100));
    volumeSlider_->setFixedWidth(140);
    // Single toggle button instead of a Waveform/Spectrogram dropdown: one
    // click switches the display mode; the label shows the current mode.
    displayToggleBtn_ = new QToolButton(playerBar);
    displayToggleBtn_->setText(Settings::instance().playerMode() == QLatin1String("spectrogram")
                                   ? tr("Spectrogram") : tr("Waveform"));
    displayToggleBtn_->setToolTip(tr("Click to switch between Waveform and Spectrogram"));
    displayToggleBtn_->setAutoRaise(false);

    pb->addWidget(prevBtn);
    pb->addWidget(playBtn_);
    pb->addWidget(stopBtn);
    pb->addWidget(nextBtn);
    pb->addWidget(loopBtn_);
    pb->addSpacing(12);
    pb->addWidget(new QLabel(tr("Volume"), playerBar));
    pb->addWidget(volumeSlider_);
    pb->addSpacing(12);
    // No "Display" label: the toggle button alone shows the current mode.
    pb->addWidget(displayToggleBtn_);
    pb->addStretch(1);
    lowerLay->addWidget(playerBar);

    connect(prevBtn, &QPushButton::clicked, this, &MainWindow::prevFile);
    connect(nextBtn, &QPushButton::clicked, this, &MainWindow::nextFile);

    // ---- playback controllers (extracted from this class) ----
    // The ffmpeg fallback talks to the UI through plain function providers so
    // it never needs to know about widgets; PlayerController owns the
    // play/pause/stop/seek/loop decisions and reports back via PlayerUi (this).
    fb_ = new FfmpegFallbackController(this);
    pc_ = new PlayerController(&player_, fb_, this, this);
    fb_->setProviders(
        [this]() -> const AudioData * { return &audio_; },
        [this]() -> qint64 { return view_->playheadFrame(); },
        [this](qint64 f) { setPlayhead(f, true); },
        [this]() -> bool { return loopBtn_->isChecked(); });
    fb_->setOnFinished([this](bool error) { pc_->onFallbackFinished(error); });
    fb_->setCurrentPathProvider([this] { return currentPath_; });

    connect(playBtn_, &QPushButton::clicked, this, [this] { pc_->playPause(); });
    connect(stopBtn, &QPushButton::clicked, this, [this] { pc_->stopPlayback(); });
    // Loop toggling: works while stopped AND while playing. Loop never sets up
    // its own repeat start point: the looped range is exactly what Play would
    // use — the current selection if there is one, otherwise the whole file.
    // While playback is active the range is re-armed immediately so looping
    // takes effect live.
    connect(loopBtn_, &QPushButton::toggled, this, [this](bool on) {
        pc_->onLoopToggled(on);
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) {
        player_.setVolume(v / 100.0);
        Settings::instance().setVolume(v / 100.0);
        // The ffmpeg direct-playback fallback is a separate process with its
        // own volume: restart it at the current position with the new gain so
        // the slider works in every output path.
        pc_->onVolumeChanged();
    });
    connect(displayToggleBtn_, &QToolButton::clicked, this, [this] {
        const bool toSpectrogram = view_->mode() != QLatin1String("spectrogram");
        const QString mode = toSpectrogram ? QStringLiteral("spectrogram")
                                           : QStringLiteral("wave");
        Settings::instance().setPlayerMode(mode);
        view_->setMode(mode);
        displayToggleBtn_->setText(toSpectrogram ? tr("Spectrogram") : tr("Waveform"));
    });

    // ---- visualization area ----
    view_ = new AudioView(lowerPane);
    lowerLay->addWidget(view_, 1);
    mainSplit->addWidget(lowerPane);
    mainSplit->setSizes({520, 340});
    mainSplit->setCollapsible(0, false);
    mainSplit->setStretchFactor(1, 1);
    view_->setMode(Settings::instance().playerMode());

    connect(view_, &AudioView::selectionChanged, this, [this](qint64, qint64) { /* info only */ });
    connect(view_, &AudioView::markerDoubleClickedEmpty, this, [this](qint64 f, bool) {
        createMarkerAt(f);
    });
    connect(view_, &AudioView::doubleClickBetweenMarkers, this, [this](qint64 a, qint64 b) {
        view_->setSelectionRange(a, b);
    });
    connect(view_, &AudioView::markerClicked, this, &MainWindow::onMarkerClicked);
    connect(view_, &AudioView::markerDoubleClicked, this, &MainWindow::openMarkerEditor);
    connect(view_, &AudioView::markerMoved, this, [this](int, qint64) { persistMarkers(); });
    connect(view_, &AudioView::cursorMoved, this, [this](qint64 f) {
        // Re-entrancy guard: programmatic cursor moves (playhead updates from
        // the position timer, Stop rewind, seek echo) must not be mistaken
        // for a user click and re-trigger a seek — that would cause an
        // infinite setPlayheadFrame → cursorMoved → seekPlaybackTo loop.
        if (seekingPlayback_) return;
        // While playing/paused, a click inside the waveform seeks: playback
        // jumps to the clicked position and continues from there.
        if (player_.isPlaying() || player_.isPaused() || pc_->fallbackActive()) {
            pc_->seekPlaybackTo(f);
            return;
        }
        // Stopped after a session: moving the cursor defines a new start
        // point for the next Play (overrides the "resume where it started"
        // behavior). W / Home jumping to 0 also flows through here.
        if (pc_->lastPlayStartFrame() >= 0) pc_->setLastPlayStartFrame(f);
        if (audio_.valid)
            Database::instance().setSessionState(currentDir_, currentPath_,
                                                 double(f) / audio_.sampleRate);
    });

    // Player engine signals are wired once, in wirePlayerSignals() below.
    wirePlayerSignals();

    previewWorker_ = new PreviewWorker(this);
    connect(previewWorker_, &PreviewWorker::ready, this, [this](const QString &path, const QString &imgPath) {
        if (imgPath.isEmpty()) return;
        QImage img(imgPath);
        if (img.isNull()) return;
        // The image path carries the generation tag it was requested under
        // ("#gN" suffix, stripped by PreviewWorker before rendering). Results
        // of a superseded mode switch/refresh are dropped so the table never
        // ends up with a mix of wave and spectrogram previews.
        const QString base = path.section(QLatin1Char('#'), 0, 0);
        const int gen = path.contains(QLatin1Char('#'))
            ? path.section(QLatin1Char('#'), 1).toInt() : -1;
        if (gen != previewGeneration_) return;   // stale result from an older fill
        const auto &rows = model_->rows();
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].path == base) { model_->setPreview(i, img); break; }
    });

}


// ---------------- hotkeys ----------------

void MainWindow::applyHotkeys() {
    // remove previous dynamic shortcuts
    const auto old = findChildren<QShortcut *>();
    for (auto *s : old) s->deleteLater();

    auto add = [&](const QString &id, std::function<void()> fn, Qt::ShortcutContext ctx) {
        QString seqStr = Settings::instance().shortcut(id);
        if (seqStr.isEmpty()) return;
        auto *sc = new QShortcut(QKeySequence(seqStr), this, nullptr, nullptr, ctx);
        connect(sc, &QShortcut::activated, this, fn);
    };

    add("mute",       [this] { doMute(); }, Qt::WindowShortcut);
    add("normalize",  [this] { doNormalize(); }, Qt::WindowShortcut);
    add("prev",       [this] { prevFile(); }, Qt::WindowShortcut);
    add("next",       [this] { nextFile(); }, Qt::WindowShortcut);
    add("undo",       [this] { undo(); }, Qt::WindowShortcut);
    add("redo",       [this] { redo(); }, Qt::WindowShortcut);
    add("gotoStart",  [this] { view_->nudgeCursor(-view_->cursorFrame()); }, Qt::WindowShortcut);
    add("zoomReset",  [this] { view_->zoomReset(); }, Qt::WindowShortcut);
    add("addMarker",  [this] { createMarkerAt(view_->cursorFrame()); }, Qt::WindowShortcut);
    add("deleteMarker",[this] {
        int idx = view_->activeMarker();
        if (idx >= 0 && idx < markers_.size()) {
            markers_.remove(idx);
            persistMarkers();
            view_->setActiveMarkerIndex(-1);
        }
    }, Qt::WindowShortcut);
    add("clearSelection",[this] { view_->clearSelection(); }, Qt::WindowShortcut);
    add("zoomIn",     [this] { view_->zoomIn(); }, Qt::WindowShortcut);
    add("zoomOut",    [this] { view_->zoomOut(); }, Qt::WindowShortcut);

    // Space is context-sensitive per settings (start/stop vs start/pause)
    QString spaceSeq = Settings::instance().shortcut("playpause");
    if (!spaceSeq.isEmpty()) {
        auto *sc = new QShortcut(QKeySequence(spaceSeq), this, nullptr, nullptr, Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, [this] {
            if (Settings::instance().spaceStartStop()) {
                if (pc_->isRunning()) pc_->stopPlayback();
                else pc_->playPause();
            } else {
                pc_->playPause();
            }
        });
    }
    // second redo binding Ctrl+Y
    auto *cy = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Y")), this);
    connect(cy, &QShortcut::activated, this, [this] { redo(); });
}

// ---------------- directory scanning ----------------

void MainWindow::onDirSelected(const QString &path) {
    QFileInfo fi(path);
    QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (dir.isEmpty() || !QDir(dir).exists()) return;
    currentDir_ = dir;

    // Background scan (spec 3.1/12): never block UI — the DirScanner worker
    // probes every audio file off-thread and delivers the rows through
    // finished() (wired in buildUi()); results of superseded scans are
    // dropped by the stale-dir check in onScanFinished().
    scanner_->scan(dir);
}

void MainWindow::onScanFinished(DirScanResult res) {
    if (res.dir != currentDir_) return;    // stale scan
    int ok = 0, brokenCnt = 0;
    for (const auto &r : res.rows) r.broken ? ++brokenCnt : ++ok;
    DBG(QStringLiteral("scan finished: dir=%1 rows=%2 ok=%3 broken=%4")
        .arg(res.dir).arg(res.rows.size()).arg(ok).arg(brokenCnt));
    model_->setRows(res.rows);
    cols_->sortByModelColumn(model_->sortColumn(), model_->sortOrder());
    // Restore the persisted header order and column widths of the file
    // table (spec 2.4): every directory scan resets the model, so apply
    // them here right after each scan instead of only once at startup.
    QTimer::singleShot(0, this, [this] {
        cols_->applySavedColumnOrder();
        cols_->loadColumnWidths();
        cols_->applyTableColumnWidths();
    });
    // lazy prefetch of first N previews (spec 2.4)
    int n = std::min<int>(Settings::instance().previewPrefetchCount(), int(res.rows.size()));
    const QString mode = Settings::instance().tablePreviewMode();
    for (int i = 0; i < n; ++i)
        if (!res.rows[i].broken)
            requestPreview(i);
    // Session restore (spec 10.2): rows exist only now, so apply the pending
    // file selection/position right after the scan of the restored directory.
    restorePendingSession();
}


// ---------------- UI wiring for the playback engine ----------------

void MainWindow::wirePlayerSignals() {
    // Audio output strategy: prefer Qt Multimedia's own QAudioSink; if the
    // default device is null or rejects our format, fall back to piping raw
    // PCM to "aplay" (ALSA) and then "ffplay" (ffmpeg). The factory returns
    // an opened write-only QIODevice; AudioPlayer wraps it in a QAudioSink.
    player_.setSinkFactory([this](const QAudioFormat &fmt) -> QIODevice * {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "MainWindow: Qt audio output unavailable, trying aplay/ffplay fallback";
        }
        if (QFileInfo::exists(QStringLiteral("/usr/bin/aplay")) ||
            !QStandardPaths::findExecutable(QStringLiteral("aplay")).isEmpty())
            return makeAplaySink(fmt, &player_);
        if (!QStandardPaths::findExecutable(QStringLiteral("ffplay")).isEmpty())
            return makeFfplaySink(fmt, &player_);
        return nullptr;
    });

    connect(&player_, &AudioPlayer::positionChanged, this, [this](qint64 f) {
        setPlayhead(f, true);
    });
    connect(&player_, &AudioPlayer::finished, this, [this] {
        playBtn_->setText(tr("Play"));
        // Natural end of a non-looped session (e.g. playback reached the end
        // of a selection): jump the visible cursor back to where the session
        // started, so it matches what the next Play will resume from — the
        // same rule Stop follows. With Loop on the range repeats forever, so
        // finished() only fires on errors — leave the cursor alone then.
        if (!loopBtn_->isChecked()) {
            setPlayhead(player_.startFrame(), true);
            pc_->setLastPlayStartFrame(player_.startFrame());  // keep resume point in sync
        }
        updatePlayButton();   // session over: drop any stale Pause/Loop styling
    });
    // Safety net: whenever the Qt sink state changes (finish, device error,
    // external stop), sync the button so it never sticks on "Pause".
    connect(&player_, &AudioPlayer::positionChanged, this,
            [this] { updatePlayButton(); });

    // Loop-button highlight must follow *actual* loop activity: re-evaluate
    // it whenever the player arms/disarms audible looping (start, stop,
    // finish, toggle) — see updatePlayButton().
    connect(&player_, &AudioPlayer::loopStateChanged, this,
            [this](bool) { updatePlayButton(); });

    // Live volume for push-mode outputs (aplay/ffplay pipe): those processes
    // read their gain only at startup, so respawn them with the new value
    // while keeping the playback position. Pull mode needs nothing: the PCM
    // generator multiplies by the atomic volume_ on every chunk.
    player_.setVolumeApplier([this](double) {
        player_.restartOutputKeepingPosition();
    });
}

void MainWindow::requestPreview(int row) {
    if (row < 0 || row >= model_->rows().size()) return;
    const auto &r = model_->rows()[row];
    if (r.broken || r.previewLoaded) return;
    // Tag the request with the current generation so results of a superseded
    // mode switch/refresh batch are discarded in the ready() handler.
    const QString tagged = r.path + QLatin1Char('#') + QString::number(previewGeneration_);
    if (previewWorker_->hasPending(tagged)) return;
    previewWorker_->request(tagged, Settings::instance().tablePreviewMode());
}

// Switch every table preview to one mode atomically: bump the generation
// counter (in-flight renders of the previous batch become stale and are
// dropped), clear the worker queue, wipe already-shown previews and start a
// fresh fill — so the Preview column never mixes wave and spectrogram rows.
void MainWindow::applyPreviewMode(const QString &mode) {
    Settings::instance().setTablePreviewMode(mode);
    ++previewGeneration_;
    previewWorker_->clearQueue();
    Database::instance().invalidatePreviewsForDir(currentDir_);
    auto mutableRows = model_->rows();
    for (auto &r : mutableRows) { r.previewLoaded = false; r.preview = QImage(); }
    model_->setRows(mutableRows);
    int n = std::min<int>(Settings::instance().previewPrefetchCount(), int(mutableRows.size()));
    for (int i = 0; i < n; ++i)
        if (!mutableRows[i].broken) requestPreview(i);
}

void MainWindow::refreshPreviewsForDir() {
    ++previewGeneration_;   // discard in-flight results of the previous batch
    previewWorker_->clearQueue();
    applyPreviewMode(Settings::instance().tablePreviewMode());
}

void MainWindow::onSearchClicked() {
    const QString needle = searchEdit_->text().trimmed().toLower();
    if (needle.isEmpty()) return;
    const auto rows = model_->rows();
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].name.toLower().contains(needle)) {
            table_->selectRow(i);
            table_->scrollTo(model_->index(i, 0));
            return;
        }
    }
    statusBar()->showMessage(tr("No match for \"%1\" in current directory").arg(needle), 4000);
}

// ---------------- file loading ----------------

int MainWindow::currentRow() const {
    auto sel = table_->selectionModel()->currentIndex();
    return sel.isValid() ? sel.row() : -1;
}

void MainWindow::onRowDoubleClicked() {
    // If a different file is selected while playback is running, stop the
    // player and reset the cursor to the beginning of the newly loaded file.
    int row = currentRow();
    if (row >= 0 && row < model_->rows().size()) {
        const QString &newPath = model_->rows()[row].path;
        if (player_.isPlaying() || fb_->isActive() || newPath != currentPath_) {
            player_.stop();
            fb_->stop();
            playBtn_->setText(tr("Play"));
        }
    }
    loadCurrentFile();
}

void MainWindow::loadCurrentFile() {
    int row = currentRow();
    if (row < 0 || row >= model_->rows().size()) return;
    const auto &r = model_->rows()[row];
    if (r.broken) {   // actions unavailable for broken files (spec 3.3)
        audio_ = AudioData{};
        currentPath_.clear();
        view_->setData(nullptr);
        updateActiveFileDisplay();
        return;
    }
    QString err;
    player_.stop();
    pc_->setLastPlayStartFrame(-1);   // per-file resume point resets on (re)load
    if (!AudioFile::load(r.path, audio_, &err)) {
        QMessageBox::warning(this, tr("Load failed"),
                             tr("Cannot open %1\n%2").arg(r.name, err));
        audio_ = AudioData{};
        view_->setData(nullptr);
        updateActiveFileDisplay();
        return;
    }
    currentPath_ = r.path;
    markers_ = Database::instance().loadMarkers(r.path);
    player_.setBuffer(audio_.samples.data(), audio_.totalFrames,
                      audio_.channels, audio_.sampleRate);
    view_->setData(&audio_);
    view_->setMarkers(&markers_);
    view_->setMode(Settings::instance().playerMode());
    undoStack_.clear();   // history is per-file/session-scope reset on load
    updateActiveFileDisplay();
}

void MainWindow::updateActiveFileDisplay() {
    const QString name = currentPath_.isEmpty()
                             ? QString()
                             : QFileInfo(currentPath_).fileName();
    if (activeFileLabel_) {
        activeFileLabel_->setText(name);
        activeFileLabel_->setToolTip(currentPath_);
        activeFileLabel_->setVisible(!name.isEmpty());
    }
    setWindowTitle(name.isEmpty() ? QStringLiteral("AudioScout")
                                  : QStringLiteral("AudioScout - %1").arg(name));
}

// ---------------- playback (delegates to PlayerController) ----------------

// PlayerUi implementation: the visible cursor. Programmatic moves set the
// re-entrancy guard so AudioView::cursorMoved never mistakes them for a user
// click-to-seek (that would loop back into PlayerController::seekPlaybackTo).
void MainWindow::setPlayhead(qint64 frame, bool programmatic) {
    if (!view_) return;
    const bool prev = seekingPlayback_;
    seekingPlayback_ = programmatic;
    view_->setPlayheadFrame(frame);
    seekingPlayback_ = prev;
}

qint64 MainWindow::playhead() const {
    return view_ ? view_->playheadFrame() : 0;
}

void MainWindow::showStatus(const QString &msg, int msec) {
    statusBar()->showMessage(msg, msec);
}

void MainWindow::rangeChanged(qint64, qint64) {
    // Informational only: the session range moved (loop retarget / seek).
}

// Keep the Play/Pause button and the Loop highlight in sync with what is
// *actually* running. The Qt sink can finish on its own (end of file, device
// error) while we believe it is playing; without this the button would stay
// stuck on "Pause". The Loop button lights only when looping is armed AND
// audible (Qt sink loop active or a running fallback session).
void MainWindow::updatePlayButton() {
    if (!playBtn_) return;
    const bool running = pc_->isRunning();
    playBtn_->setText(running ? tr("Pause") : tr("Play"));

    const bool active = loopBtn_->isChecked() &&
                        (player_.isLoopActive() ||
                         (pc_->fallbackActive() && pc_->fallbackWasPlaying()));
    static bool firstLoopStyle = true;
    if (firstLoopStyle) {
        loopBtn_->setAutoFillBackground(true);
        firstLoopStyle = false;
    }
    const QPalette pal = active
        ? loopBtn_->property("litPalette").value<QPalette>()
        : loopBtn_->property("basePalette").value<QPalette>();
    loopBtn_->setPalette(pal);
}

// Click-to-seek during an active session: forwards to the controller, which
// knows which output path (Qt sink vs ffmpeg fallback) is live.
void MainWindow::seekPlaybackTo(qint64 frame) {
    pc_->seekPlaybackTo(frame);
}


void MainWindow::prevFile() {
    int row = currentRow();
    if (row > 0) {
        if (player_.isPlaying() || fb_->isActive()) {
            player_.stop();
            fb_->stop();
            playBtn_->setText(tr("Play"));
        }
        table_->selectRow(row - 1);
        table_->scrollTo(model_->index(row - 1, 0));
        loadCurrentFile();   // prev/next load the file immediately
    } else if (row == 0 && audio_.valid) {
        // Already at the first file: rewind to the start so repeated Prev
        // behaves predictably instead of silently doing nothing.
        view_->setPlayheadFrame(0);
    }
}

void MainWindow::nextFile() {
    int row = currentRow();
    if (row >= 0 && row + 1 < model_->rows().size()) {
        if (player_.isPlaying() || fb_->isActive()) {
            player_.stop();
            fb_->stop();
            playBtn_->setText(tr("Play"));
        }
        table_->selectRow(row + 1);
        table_->scrollTo(model_->index(row + 1, 0));
        loadCurrentFile();   // prev/next load the file immediately
    } else {
        pc_->stopPlayback();   // end of list -> stop (spec 2.6)
    }
}

// ---------------- editing ----------------

bool MainWindow::editGuarded() {
    if (!audio_.valid || currentPath_.isEmpty()) {
        statusBar()->showMessage(tr("No active file"), 3000);
        return false;
    }
    if (!isEditableFormat(currentPath_.toLower())) {
        QMessageBox::information(this, tr("Not editable"),
            tr("Editing is available only for WAV/AIFF/CAF PCM files."));
        return false;
    }
    if (!QFileInfo(currentPath_).isWritable()) {
        QMessageBox::warning(this, tr("Read-only"),
            tr("You have no write permission for this file."));
        return false;
    }
    return true;
}

void MainWindow::applyEditToRange(std::function<void()> op, const QString &type,
                                  qint64 off, qint64 len,
                                  const QVector<UndoRecord::MarkerDelta> &deltas) {
    QString err;
    UndoRecord rec;
    rec.type = type;
    rec.filePath = currentPath_;
    rec.offset = off;
    rec.length = len;
    rec.newLength = len;
    rec.channels = audio_.channels;
    rec.timestamp = QDateTime::currentDateTime();
    rec.markerDeltas = deltas;
    // original samples of the affected range are stored on disk BEFORE editing
    rec.samplesFile = UndoStack::writeOriginalSamples(
        Settings::instance().undoDir(), audio_, off, off + len, &err);
    if (rec.samplesFile.isEmpty()) {
        QMessageBox::warning(this, tr("Undo error"), err);
        return;
    }
    // Remember the normalize target level so redo() re-applies the SAME gain
    // even if the user changes the spinbox in between.
    if (type == QLatin1String("normalize"))
        rec.normalizeDbfs = normLevel_->value();
    op();
    // Save FIRST: only if the file was actually written do we record the undo
    // entry. Otherwise a failed save would leave history/buffer in conflict
    // with the disk (undo would restore samples that were never persisted).
    if (!AudioFile::save(currentPath_, audio_, &err)) {
        QFile::remove(rec.samplesFile);   // drop the orphaned sample dump
        // Roll the in-memory buffer back to its pre-edit state so it stays in
        // sync with the untouched file on disk; otherwise subsequent edits or
        // an accidental later save would persist changes the user believes
        // failed, and undo/redo would operate on phantom data.
        std::vector<float> orig;
        QString rerr;
        if (UndoStack::readOriginalSamples(rec.samplesFile, orig, &rerr) &&
            qint64(orig.size()) == len * audio_.channels) {
            std::copy(orig.begin(), orig.end(),
                      std::next(audio_.samples.begin(), off * audio_.channels));
        } else {
            // Cannot roll back reliably — reload the file from disk instead.
            AudioData fresh;
            if (AudioFile::load(currentPath_, fresh, nullptr) && fresh.valid)
                audio_ = fresh;
        }
        player_.setBuffer(audio_.samples.data(), audio_.totalFrames,
                          audio_.channels, audio_.sampleRate);
        view_->setData(&audio_);
        view_->update();
        QMessageBox::warning(this, tr("Save failed"), err);
        return;
    }
    undoStack_.push(rec);
    undoStack_.enforceLimits(Settings::instance().undoDepth(),
                             Settings::instance().undoDiskLimitBytes());
    persistMarkers();
    // file changed: invalidate its preview cache entry and re-request preview
    Database::instance().invalidatePreviewsForDir(currentDir_);
    {
        auto rows = model_->rows();
        int row = currentRow();
        if (row >= 0 && row < rows.size()) {
            FileTableModel::Row upd = rows[row];
            upd.previewLoaded = false;
            upd.preview = QImage();
            upd.frames = audio_.totalFrames;
            upd.size = QFileInfo(currentPath_).size();
            upd.modified = QFileInfo(currentPath_).lastModified();
            model_->updateRow(row, upd);   // in-place refresh, keeps selection intact
            requestPreview(row);
        }
    }
}

void MainWindow::doMute() {
    if (!editGuarded()) return;
    qint64 off = 0, len = audio_.totalFrames;
    if (view_->hasSelection()) { off = view_->selStart(); len = view_->selEnd() - off; }
    const int ch = audio_.channels;
    applyEditToRange([&] {
        std::fill(audio_.samples.begin() + off * ch,
                  audio_.samples.begin() + (off + len) * ch, 0.0f);
    }, QStringLiteral("mute"), off, len);
    view_->update();
}

void MainWindow::doNormalize() {
    if (!editGuarded()) return;
    qint64 off = 0, len = audio_.totalFrames;
    if (view_->hasSelection()) { off = view_->selStart(); len = view_->selEnd() - off; }
    const double dbfs = normLevel_->value();
    const float targetPeak = float(std::pow(10.0, dbfs / 20.0));
    const int ch = audio_.channels;
    applyEditToRange([&] {
        float peak = 0.f;
        for (qint64 i = off * ch; i < (off + len) * ch; ++i)
            peak = std::max(peak, std::abs(audio_.samples[i]));
        if (peak <= 0.f) return;
        const float gain = targetPeak / peak;
        for (qint64 i = off * ch; i < (off + len) * ch; ++i)
            audio_.samples[i] *= gain;
    }, QStringLiteral("normalize"), off, len);
    view_->update();
}

void MainWindow::doDeleteSelection() {
    if (!editGuarded()) return;
    if (!view_->hasSelection()) {   // unavailable without selection (spec 8.4)
        statusBar()->showMessage(tr("Delete selection requires an active selection"), 3000);
        return;
    }
    const qint64 off = view_->selStart();
    const qint64 len = view_->selEnd() - off;
    const int ch = audio_.channels;

    // capture original samples BEFORE modifying buffer
    QString err;
    QString samplesFile = UndoStack::writeOriginalSamples(
        Settings::instance().undoDir(), audio_, off, off + len, &err);
    if (samplesFile.isEmpty()) {
        QMessageBox::warning(this, tr("Undo error"), err);
        return;
    }

    // marker bookkeeping (spec 4.6)
    QVector<UndoRecord::MarkerDelta> deltas;
    for (auto &m : markers_) {
        UndoRecord::MarkerDelta d;
        d.m = m; d.oldPos = m.timeSamples;
        if (m.timeSamples >= off && m.timeSamples < off + len) {
            d.deleted = true;
            d.newPos = m.timeSamples;
        } else if (m.timeSamples >= off + len) {
            d.deleted = false;
            d.newPos = m.timeSamples - len;
        } else {
            d.deleted = false;
            d.newPos = m.timeSamples;
        }
        deltas.push_back(d);
    }

    applyEditToRange([&] {
        // shift remaining samples left
        audio_.samples.erase(std::next(audio_.samples.begin(), off * ch),
                             std::next(audio_.samples.begin(), (off + len) * ch));
        audio_.totalFrames -= len;
        // apply marker changes
        for (int i = 0; i < markers_.size(); ++i) {
            const auto &d = deltas[i];
            if (d.deleted) continue;
            markers_[i].timeSamples = d.newPos;
            markers_[i].timeSec = double(d.newPos) / audio_.sampleRate;
        }
        // erase deleted markers from vector (iterate reverse)
        for (int i = markers_.size() - 1; i >= 0; --i)
            if (deltas[i].deleted) markers_.removeAt(i);
        view_->clearSelection();
    }, QStringLiteral("delete"), off, len, deltas);

    view_->update();
}

// ---------------- undo / redo ----------------

void MainWindow::undo() {
    const UndoRecord *rec = undoStack_.takeUndo();
    if (!rec || rec->filePath != currentPath_ || !audio_.valid) {
        statusBar()->showMessage(tr("Nothing to undo"), 2000);
        return;
    }
    QString err;
    const int ch = audio_.channels;
    if (rec->type == QLatin1String("delete")) {
        // re-insert removed samples
        std::vector<float> orig;
        if (!UndoStack::readOriginalSamples(rec->samplesFile, orig, &err)) {
            QMessageBox::warning(this, tr("Undo"), err); return;
        }
        audio_.samples.insert(std::next(audio_.samples.begin(), rec->offset * ch),
                              orig.begin(), orig.end());
        audio_.totalFrames += rec->length;
        // restore markers: undeleted ones return at old positions, deleted are revived
        markers_.clear();
        for (const auto &d : rec->markerDeltas) {
            Marker m = d.m;
            m.timeSamples = d.oldPos;
            m.timeSec = double(d.oldPos) / audio_.sampleRate;
            markers_.push_back(m);   // spec 4.6: undo restores markers at original positions
        }
    } else {
        std::vector<float> orig;
        if (!UndoStack::readOriginalSamples(rec->samplesFile, orig, &err)) {
            QMessageBox::warning(this, tr("Undo"), err); return;
        }
        std::copy(orig.begin(), orig.end(),
                  std::next(audio_.samples.begin(), rec->offset * ch));
    }
    player_.setBuffer(audio_.samples.data(), audio_.totalFrames, audio_.channels, audio_.sampleRate);
    if (!AudioFile::save(currentPath_, audio_, &err))
        QMessageBox::warning(this, tr("Save failed"), err);
    persistMarkers();
    view_->update();
}

void MainWindow::redo() {
    const UndoRecord *rec = undoStack_.takeRedo();
    if (!rec || rec->filePath != currentPath_ || !audio_.valid) {
        statusBar()->showMessage(tr("Nothing to redo"), 2000);
        return;
    }
    // Re-apply operation using stored metadata.
    QString err;
    const int ch = audio_.channels;
    if (rec->type == QLatin1String("mute")) {
        std::fill(audio_.samples.begin() + rec->offset * ch,
                  audio_.samples.begin() + (rec->offset + rec->length) * ch, 0.0f);
    } else if (rec->type == QLatin1String("normalize")) {
        // Re-apply the gain with the target level captured when the operation
        // was performed — not whatever the spinbox shows now.
        const double dbfs = rec->normalizeDbfs;
        float target = float(std::pow(10.0, dbfs / 20.0));
        float peak = 0.f;
        for (qint64 i = rec->offset * ch; i < (rec->offset + rec->length) * ch; ++i)
            peak = std::max(peak, std::abs(audio_.samples[i]));
        // after undo the restored original is in place, so re-normalize it
        if (peak > 0.f) {
            float gain = target / peak;
            for (qint64 i = rec->offset * ch; i < (rec->offset + rec->length) * ch; ++i)
                audio_.samples[i] *= gain;
        }
    } else if (rec->type == QLatin1String("delete")) {
        audio_.samples.erase(std::next(audio_.samples.begin(), rec->offset * ch),
                             std::next(audio_.samples.begin(), (rec->offset + rec->length) * ch));
        audio_.totalFrames -= rec->length;
        markers_.clear();
        for (const auto &d : rec->markerDeltas) {
            if (d.deleted) continue;
            Marker m = d.m;
            m.timeSamples = d.newPos;
            m.timeSec = double(d.newPos) / audio_.sampleRate;
            markers_.push_back(m);
        }
    }
    player_.setBuffer(audio_.samples.data(), audio_.totalFrames, audio_.channels, audio_.sampleRate);
    if (!AudioFile::save(currentPath_, audio_, &err))
        QMessageBox::warning(this, tr("Save failed"), err);
    persistMarkers();
    view_->update();
}

// ---------------- markers ----------------

void MainWindow::persistMarkers() {
    QFileInfo fi(currentPath_);
    Database::instance().saveMarkers(currentPath_, fi.size(),
                                     fi.lastModified().toMSecsSinceEpoch(), markers_);
}

void MainWindow::createMarkerAt(qint64 frame) {
    if (!audio_.valid) return;
    Marker m;
    m.name = QStringLiteral("M%1").arg(markers_.size() + 1);
    m.timeSamples = std::clamp(frame, qint64(0), audio_.totalFrames);
    m.timeSec = double(m.timeSamples) / audio_.sampleRate;
    markers_.push_back(m);
    persistMarkers();
    view_->setActiveMarkerIndex(markers_.size() - 1);
    view_->update();
}

void MainWindow::onMarkerClicked(int idx) {
    // Single click only highlights the marker. The editor dialog is opened
    // through AudioView::markerDoubleClicked (reliable double-click detection
    // inside the view's press handler), connected below.
    if (idx < 0 || idx >= markers_.size()) return;
    view_->setActiveMarkerIndex(idx);
}

void MainWindow::openMarkerEditor(int idx) {
    if (idx < 0 || idx >= markers_.size()) return;
    view_->setActiveMarkerIndex(idx);
    Marker copy = markers_[idx];
    MarkerDialog dlg(copy, audio_.sampleRate, this);
    dlg.exec();
    if (dlg.deleteRequested()) {
        markers_.remove(idx);
        view_->setActiveMarkerIndex(-1);
    } else {
        markers_[idx] = dlg.marker();
        markers_[idx].timeSec = double(markers_[idx].timeSamples) / audio_.sampleRate;
    }
    persistMarkers();
    view_->update();
}

// ---------------- rename ----------------

void MainWindow::renameCurrentFile() {
    int row = currentRow();
    if (row < 0 || !QFileInfo(currentPath_).isWritable()) {
        QMessageBox::warning(this, tr("Rename"), tr("File is not writable or not selected."));
        return;
    }
    bool ok = false;
    QString newName = QInputDialog::getText(this, tr("Rename file"), tr("New name:"),
                                            QLineEdit::Normal, QFileInfo(currentPath_).fileName(),
                                            &ok);
    if (!ok || newName.isEmpty()) return;
    QString newPath = QFileInfo(currentPath_).absolutePath() + "/" + newName;
    if (QFile::rename(currentPath_, newPath)) {
        Database::instance().renameFilePath(currentPath_, newPath);
        currentPath_ = newPath;
        updateActiveFileDisplay();     // reflect the new name immediately
        onDirSelected(currentDir_);   // rescan updates paths in the table
    } else {
        QMessageBox::warning(this, tr("Rename"), tr("Rename failed."));
    }
}

void MainWindow::deleteAllMarkersForFile() {
    if (currentPath_.isEmpty()) return;
    markers_.clear();
    Database::instance().deleteMarkersForFile(currentPath_);
    view_->update();
}

// ---------------- settings ----------------

void MainWindow::showSettings() {
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::hotkeysChanged, this, &MainWindow::applyHotkeys);
    dlg.exec();
    view_->setMode(Settings::instance().playerMode());
    tableModeBtn_->setText(Settings::instance().tablePreviewMode() == QLatin1String("spectrogram")
                               ? tr("Spectrogram") : tr("Waveform"));
}
