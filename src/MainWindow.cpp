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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
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
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QMediaDevices>
#include <QProcess>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>

// ---------------- debug logging (playback troubleshooting) ----------------
// Writes to ~/.config/AudioScout/audioscout_debug.log so the log is found
// regardless of the directory the app was launched from.
static QFile     g_dbgFile;
static QTextStream g_dbg(&g_dbgFile);
static bool      g_dbgOn = false;

static void dbgOpenIfNeeded() {
    if (g_dbgOn) return;
    const QDir cfgDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    if (!cfgDir.exists()) cfgDir.mkpath(QStringLiteral("."));
    g_dbgFile.setFileName(cfgDir.filePath(QStringLiteral("audioscout_debug.log")));
    if (g_dbgFile.open(QIODevice::Append | QIODevice::Text)) {
        g_dbg.setDevice(&g_dbgFile);
        g_dbgOn = true;
        g_dbg << "\n===== session start " << QDateTime::currentDateTime().toString(Qt::ISODate)
              << " pid " << QCoreApplication::applicationPid() << " =====\n";
        g_dbg.flush();
    }
}

#define DBG(msg) do { \
    dbgOpenIfNeeded(); \
    qWarning().noquote() << "[audioscout]" << (msg); \
    if (g_dbgOn) { \
        g_dbg << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << "  " << (msg) << "\n"; \
        g_dbg.flush(); \
    } \
} while (0)

// Fallback sinks: when Qt Multimedia has no usable audio output device
// (missing PulseAudio/PipeWire plugins), pipe raw PCM to an external player
// via a QProcess-backed QIODevice. QAudioSink accepts any QIODevice that
// supports write, so this keeps playback working even in bare environments.
class ProcessSink : public QIODevice {
public:
    explicit ProcessSink(const QString &program, const QStringList &args,
                         QObject *parent = nullptr)
        : QIODevice(parent), program_(program), args_(args) {}
    ~ProcessSink() override { shutdown(); }

    // Restart the child process with an updated argument list while keeping
    // the same object identity (AudioPlayer deletes this device only when it
    // replaces it). Used for live volume changes on "aplay", whose -volume is
    // fixed at spawn time. The next writeData() respawns lazily with newArgs.
    void restartWithArgs(const QStringList &newArgs) {
        args_ = newArgs;
        shutdown();
    }

protected:
    qint64 readData(char *, qint64) override { return 0; }
    qint64 writeData(const char *data, qint64 len) override {
        if (!ensureStarted()) return -1;
        proc_->write(data, len);
        proc_->waitForBytesWritten(500);
        return len;
    }
    void close() override { shutdown(); }

private:
    bool ensureStarted() {
        if (proc_) {
            if (proc_->state() == QProcess::NotRunning &&
                proc_->exitStatus() != QProcess::NormalExit)
                return false;   // crashed earlier — don't retry
            return proc_->state() == QProcess::Running;
        }
        proc_ = new QProcess(this);
        proc_->setProgram(program_);
        proc_->setArguments(args_);
        proc_->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        proc_->start();
        if (!proc_->waitForStarted(3000)) {
            qWarning().noquote()
                << QStringLiteral("AudioPlayer: failed to start '%1'").arg(program_);
            delete proc_;
            proc_ = nullptr;
            return false;
        }
        return true;
    }
    void shutdown() {
        if (!proc_) return;
        proc_->closeWriteChannel();
        proc_->waitForFinished(300);
        if (proc_->state() == QProcess::Running) {
            proc_->terminate();
            if (!proc_->waitForFinished(300)) proc_->kill();
        }
    }
    QString program_;
    QStringList args_;
    QProcess *proc_ = nullptr;
};

static QStringList aplayArgsFor(const QAudioFormat &fmt, double volume) {
    auto tag = [](QAudioFormat::SampleFormat sf) -> QString {
        switch (sf) {
        case QAudioFormat::Int16: return QStringLiteral("S16_LE");
        case QAudioFormat::Int32: return QStringLiteral("S32_LE");
        case QAudioFormat::Float: return QStringLiteral("F32_LE");
        case QAudioFormat::UInt8: return QStringLiteral("U8");
        default:                  return QStringLiteral("S16_LE");
        }
    };
    QStringList args;
    args << "-q" << "-t" << "raw"
         << "-c" << QString::number(fmt.channelCount())
         << "-r" << QString::number(fmt.sampleRate())
         << "-f" << tag(fmt.sampleFormat());
    // aplay applies its own hardware-independent gain; the in-process float
    // path multiplies by volume_ directly, so do the same here. 0 dB = full
    // scale; anything above 0 dB clips, so clamp to it.
    const double db = volume >= 0.999 ? 0.0
                     : 20.0 * std::log10(std::max(volume, 0.0005));
    args << "-v" << QString::number(db, 'f', 1);
    return args;
}

static QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent) {
    auto *s = new ProcessSink(QStringLiteral("aplay"), aplayArgsFor(fmt, 1.0), parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}

static QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent) {
    QStringList args;
    // NOTE: ffplay has no -autoexit option; it exits on EOF of stdin input.
    args << "-nodisp" << "-loglevel" << "quiet"
         << "-f" << "f32le"
         << "-ar" << QString::number(fmt.sampleRate())
         << "-ac" << QString::number(fmt.channelCount())
         << "-i" << "-";
    auto *s = new ProcessSink(QStringLiteral("ffplay"), args, parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}


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
};

// ---------------- helpers ----------------

// ---------------- construction ----------------

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    buildUi();
    applyHotkeys();
    connect(&Settings::instance(), &Settings::changed, this, &MainWindow::applyHotkeys);

    // restore last session state
    QString dir, file; double pos;
    Database::instance().getSessionState(dir, file, pos);
    if (!dir.isEmpty() && QDir(dir).exists()) {
        tree_->setCurrentIndex(fsModel_->index(dir));
        onDirSelected(dir);
    } else {
        onDirSelected(QDir::homePath());
        tree_->setCurrentIndex(fsModel_->index(QDir::homePath()));
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *e) {
    player_.stop();
    Database::instance().setSessionState(
        currentDir_, currentPath_,
        audio_.sampleRate ? double(view_->cursorFrame()) / audio_.sampleRate : 0.0);
    e->accept();
}

// ---------------- UI ----------------

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("AudioScout"));
    resize(1280, 860);

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
    columnsBtn_->setPopupMode(QToolButton::InstantPopup);
    tb->addWidget(columnsBtn_);

    tableModeCombo_ = new QComboBox(tb);
    tableModeCombo_->addItems({tr("Wave preview"), tr("Spectrogram preview")});
    tableModeCombo_->setCurrentIndex(Settings::instance().tablePreviewMode() == QLatin1String("spectrogram") ? 1 : 0);
    tb->addWidget(new QLabel(tr(" Table: "), tb));
    tb->addWidget(tableModeCombo_);

    actRefreshPrev_ = new QAction(tr("Refresh previews"), tb);
    tb->addAction(actRefreshPrev_);

    actZoomIn_ = tb->addAction(tr("+Z"));
    actZoomOut_ = tb->addAction(tr("-Z"));

    actSettings_ = tb->addAction(tr("Settings"));

    connect(actRefreshPrev_, &QAction::triggered, this, &MainWindow::refreshPreviewsForDir);
    connect(actZoomIn_, &QAction::triggered, view_, &AudioView::zoomIn);
    connect(actZoomOut_, &QAction::triggered, view_, &AudioView::zoomOut);
    connect(actSettings_, &QAction::triggered, this, &MainWindow::showSettings);
    connect(tableModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        Settings::instance().setTablePreviewMode(idx == 1 ? QStringLiteral("spectrogram")
                                                          : QStringLiteral("wave"));
        refreshPreviewsForDir();
    });

    // ---- splitter: tree | table ----
    auto *mainSplit = new QSplitter(Qt::Vertical, central);
    outer->addWidget(mainSplit, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, mainSplit);

    tree_ = new QTreeView(splitter);
    tree_->setModel(fsModel_);
    tree_->setRootIndex(fsModel_->index(QDir::rootPath()));
    tree_->setHeaderHidden(true);
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
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex &i) {
        onDirSelected(fsModel_->filePath(i));
    });
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
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, [this](int visual) {
        const int logical = table_->horizontalHeader()->logicalIndex(visual);
        if (logical < 0) return;
        const int modelCol = model_->visibleColumns().value(logical, -1);
        if (modelCol == FileTableModel::ColPreview ||
            modelCol == FileTableModel::ColComment) {
            // Sorting forbidden per spec 2.4 — keep the current indicator.
            updateSortIndicator();
            return;
        }
        Qt::SortOrder order = Qt::AscendingOrder;
        if (modelCol == model_->sortColumn())
            order = (model_->sortOrder() == Qt::AscendingOrder)
                        ? Qt::DescendingOrder : Qt::AscendingOrder;
        sortByModelColumn(modelCol, order);
    });
    // After a header drag-reorder or any model reset/sort, re-apply sensible
    // widths to the visual sections (model columns may have moved around).
    connect(table_->horizontalHeader(), &QHeaderView::sectionMoved,
            this, [this] { applyTableColumnWidths(); updateSortIndicator(); });
    connect(model_, &QAbstractItemModel::modelReset,
            this, [this] { QTimer::singleShot(0, this, [this] { applyTableColumnWidths(); }); });
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
    ab->addStretch(1);
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
    // Highlight the button while looping is active.
    {
        QPalette pal = loopBtn_->palette();
        const QColor accent(70, 130, 220);
        pal.setColor(QPalette::Button, accent);
        pal.setColor(QPalette::ButtonText, Qt::white);
        pal.setColor(QPalette::Highlight, accent.darker(120));
        loopBtn_->setAutoFillBackground(true);
        loopBtn_->setPalette(pal);
    }
    volumeSlider_ = new QSlider(Qt::Horizontal, playerBar);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(int(Settings::instance().volume() * 100));
    volumeSlider_->setFixedWidth(140);
    playerModeCombo_ = new QComboBox(playerBar);
    playerModeCombo_->addItems({tr("Waveform"), tr("Spectrogram")});
    playerModeCombo_->setCurrentIndex(Settings::instance().playerMode() == QLatin1String("spectrogram") ? 1 : 0);
    auto *zin = new QPushButton(tr("Zoom +"), playerBar);
    auto *zout = new QPushButton(tr("Zoom -"), playerBar);

    pb->addWidget(prevBtn);
    pb->addWidget(playBtn_);
    pb->addWidget(stopBtn);
    pb->addWidget(nextBtn);
    pb->addWidget(loopBtn_);
    pb->addSpacing(12);
    pb->addWidget(new QLabel(tr("Volume"), playerBar));
    pb->addWidget(volumeSlider_);
    pb->addSpacing(12);
    pb->addWidget(new QLabel(tr("Display"), playerBar));
    pb->addWidget(playerModeCombo_);
    pb->addStretch(1);
    pb->addWidget(zin);
    pb->addWidget(zout);
    lowerLay->addWidget(playerBar);

    connect(prevBtn, &QPushButton::clicked, this, &MainWindow::prevFile);
    connect(nextBtn, &QPushButton::clicked, this, &MainWindow::nextFile);
    connect(playBtn_, &QPushButton::clicked, this, &MainWindow::playPause);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    // Loop toggling: works while stopped AND while playing. When playback is
    // active the range is re-armed immediately so looping takes effect live.
    connect(loopBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (player_.isPlaying() || player_.isPaused()) {
            player_.setLooping(on);
            return;   // Qt sink loops internally, no restart needed
        }
        if (playProc_) {
            // ffmpeg fallback is running: restart it over the same range with
            // the new loop flag, resuming from the current playhead position.
            qint64 cur = view_->playheadFrame();
            if (cur < fbStartFrame_ || cur >= fbEndFrame_) cur = fbStartFrame_;
            startFfmpegFallback(cur, fbEndFrame_, on);
            if (playProc_) playBtn_->setText(tr("Pause"));
        }
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) {
        player_.setVolume(v / 100.0);
        Settings::instance().setVolume(v / 100.0);
    });
    connect(playerModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        QString mode = idx == 1 ? QStringLiteral("spectrogram") : QStringLiteral("wave");
        Settings::instance().setPlayerMode(mode);
        view_->setMode(mode);
    });
    connect(zin, &QPushButton::clicked, view_, &AudioView::zoomIn);
    connect(zout, &QPushButton::clicked, view_, &AudioView::zoomOut);

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
    connect(view_, &AudioView::markerMoved, this, [this](int, qint64) { persistMarkers(); });
    connect(view_, &AudioView::cursorMoved, this, [this](qint64 f) {
        // While playing/paused, a click inside the waveform seeks: playback
        // jumps to the clicked position and continues from there.
        if (player_.isPlaying() || player_.isPaused() || playProc_) {
            seekPlaybackTo(f);
            return;
        }
        if (audio_.valid)
            Database::instance().setSessionState(currentDir_, currentPath_,
                                                 double(f) / audio_.sampleRate);
    });

    connect(&player_, &AudioPlayer::positionChanged, this, [this](qint64 f) {
        view_->setPlayheadFrame(f);
    });
    connect(&player_, &AudioPlayer::finished, this, [this] {
        playBtn_->setText(tr("Play"));
    });
    // Safety net: whenever the Qt sink state changes (finish, device error,
    // external stop), sync the button so it never sticks on "Pause".
    connect(&player_, &AudioPlayer::positionChanged, this,
            [this] { updatePlayButton(); });

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

    previewWorker_ = new PreviewWorker(this);
    connect(previewWorker_, &PreviewWorker::ready, this, [this](const QString &path, const QString &imgPath) {
        if (imgPath.isEmpty()) return;
        QImage img(imgPath);
        if (img.isNull()) return;
        const auto &rows = model_->rows();
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].path == path) { model_->setPreview(i, img); break; }
    });

    rebuildColumnMenu();
}

void MainWindow::rebuildColumnMenu() {
    auto *menu = new QMenu(columnsBtn_);
    const QStringList order = Settings::instance().visibleColumns();
    QVector<int> visible;
    for (const QString &key : order) {
        int col = FileTableModel::columnFromKey(key);
        if (col >= 0) visible << col;
    }
    for (int c = 0; c < int(FileTableModel::ColCount); ++c)
        if (!visible.contains(c)) visible << c;

    for (int c = 0; c < int(FileTableModel::ColCount); ++c) {
        auto *act = menu->addAction(model_->columnName(c));
        act->setCheckable(true);
        act->setChecked(visible.contains(c));
    }
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        // sync check states from settings
    });
    connect(menu, &QMenu::triggered, this, [this, menu](QAction *) {
        QVector<int> vis;
        const auto acts = menu->actions();
        for (int cc = 0; cc < acts.size(); ++cc)
            if (acts[cc]->isChecked()) vis << cc;
        model_->updateVisibleColumns(vis);
        QStringList keys;
        for (int cc : vis) keys << FileTableModel::columnKey(cc);
        Settings::instance().setVisibleColumns(keys);
    });
    columnsBtn_->setMenu(menu);
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
                const bool running = player_.isPlaying() || player_.isPaused() || playProc_;
                if (running) stopPlayback();
                else playPause();
            } else {
                playPause();
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

    // Background scan (spec 3.1/12): never block UI.
    auto future = QtConcurrent::run([dir]() {
        QVector<FileTableModel::Row> rows;
        QDir d(dir);
        const auto entries = d.entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo &fi : entries) {
            if (!isAudioFile(fi.absoluteFilePath().toLower())) continue;
            FileTableModel::Row r;
            r.path = fi.absoluteFilePath();
            r.name = fi.fileName();
            r.size = fi.size();
            // birthTime is often invalid on Linux filesystems; fall back to ctime.
            r.created = fi.birthTime().isValid() ? fi.birthTime() : fi.metadataChangeTime();
            if (!r.created.isValid()) r.created = fi.fileTime(QFileDevice::FileBirthTime);
            r.modified = fi.lastModified();
            AudioData meta;
            QString err;
            if (AudioFile::probe(r.path, meta, &err) && meta.valid) {
                r.broken = false;
                r.sampleRate = meta.sampleRate;
                r.channels = meta.channels;
                r.frames = meta.totalFrames;
                r.sampleTypeName = meta.format == SampleFormat::Unknown
                    ? QStringLiteral("compressed") : sampleFormatName(meta.format);
                r.comment = meta.comment;
            } else {
                qWarning() << "scanDirectory: probe failed for" << r.path << "-" << err;
                r.broken = true;   // red row, no params, no preview (spec 3.3)
            }
            rows.push_back(r);
        }
        return rows;
    });
    // poll-free completion via watcher
    auto *watcher = new QFutureWatcher<QVector<FileTableModel::Row>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, dir]() {
        DirScanResult res2{dir, watcher->result()};
        watcher->deleteLater();
        onScanFinished(res2);
    });
    watcher->setFuture(future);
}

void MainWindow::onScanFinished(DirScanResult res) {
    if (res.dir != currentDir_) return;    // stale scan
    int ok = 0, brokenCnt = 0;
    for (const auto &r : res.rows) r.broken ? ++brokenCnt : ++ok;
    DBG(QStringLiteral("scan finished: dir=%1 rows=%2 ok=%3 broken=%4")
        .arg(res.dir).arg(res.rows.size()).arg(ok).arg(brokenCnt));
    model_->setRows(res.rows);
    sortByModelColumn(model_->sortColumn(), model_->sortOrder());
    // lazy prefetch of first N previews (spec 2.4)
    int n = std::min<int>(Settings::instance().previewPrefetchCount(), int(res.rows.size()));
    const QString mode = Settings::instance().tablePreviewMode();
    for (int i = 0; i < n; ++i)
        if (!res.rows[i].broken)
            requestPreview(i);
}

// Sort the model by a *model* column id and keep the header indicator,
// widths and the selected row in sync afterwards.
void MainWindow::sortByModelColumn(int modelCol, Qt::SortOrder order) {
    // Remember the selection by file path — sorting resets the model, so
    // row indices are not stable across it.
    QString selPath;
    const int row = currentRow();
    if (row >= 0 && row < model_->rows().size())
        selPath = model_->rows()[row].path;

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
void MainWindow::updateSortIndicator() {
    if (!table_ || !model_) return;
    const int vis = model_->visibleColumns().indexOf(model_->sortColumn());
    if (vis < 0) { table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder); return; }
    table_->horizontalHeader()->setSortIndicator(
        table_->horizontalHeader()->visualIndex(vis), model_->sortOrder());
}

// Set a comfortable width for every visible section, mapping the visual
// section back to its model column via the header's logical index.
void MainWindow::applyTableColumnWidths() {
    static const int kColW[FileTableModel::ColCount] =
        {220, 350, 70, 90, 90, 80, 90, 140, 130, 130};
    if (!table_ || !model_) return;
    for (int vis = 0; vis < model_->columnCount(); ++vis) {
        int logical = table_->horizontalHeader()->logicalIndex(vis);
        if (logical < 0) continue;
        int modelCol = model_->visibleColumns().value(logical, -1);
        if (modelCol >= 0 && modelCol < int(FileTableModel::ColCount))
            table_->setColumnWidth(logical, kColW[modelCol]);
    }
}

void MainWindow::requestPreview(int row) {
    if (row < 0 || row >= model_->rows().size()) return;
    const auto &r = model_->rows()[row];
    if (r.broken || r.previewLoaded) return;
    if (previewWorker_->hasPending(r.path)) return;
    previewWorker_->request(r.path, Settings::instance().tablePreviewMode());
}

void MainWindow::refreshPreviewsForDir() {
    Database::instance().invalidatePreviewsForDir(currentDir_);
    auto mutableRows = model_->rows();
    for (auto &r : mutableRows) { r.previewLoaded = false; r.preview = QImage(); }
    model_->setRows(mutableRows);
    int n = std::min<int>(Settings::instance().previewPrefetchCount(), int(mutableRows.size()));
    for (int i = 0; i < n; ++i)
        if (!mutableRows[i].broken) requestPreview(i);
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
        if (player_.isPlaying() || playProc_ || newPath != currentPath_) {
            player_.stop();
            stopFfmpegFallback();
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
        return;
    }
    QString err;
    player_.stop();
    if (!AudioFile::load(r.path, audio_, &err)) {
        QMessageBox::warning(this, tr("Load failed"),
                             tr("Cannot open %1\n%2").arg(r.name, err));
        audio_ = AudioData{};
        view_->setData(nullptr);
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
}

// ---------------- playback ----------------

void MainWindow::playPause() {
    if (!audio_.valid) return;
    if (player_.isPlaying()) {          // in-process Qt sink is running
        player_.pause();
        playBtn_->setText(tr("Play"));
        return;
    }
    if (player_.isPaused()) {           // resume Qt playback where we left off
        player_.resume();
        playBtn_->setText(tr("Pause"));
        return;
    }
    if (playProc_) {                     // external fallback is running: pause it
        qint64 cur = view_->playheadFrame();
        stopFfmpegFallback();            // keeps fbStart/End/Loop, resets wasPlaying_
        if (cur < fbStartFrame_ || cur >= fbEndFrame_) cur = fbStartFrame_;
        view_->setPlayheadFrame(cur);    // resume point remembered by -ss below
        playBtn_->setText(tr("Play"));
        return;
    }
    // Paused ffmpeg fallback: resume from the playhead position.
    if (fbStartFrame_ > 0 || fbEndFrame_ > 0) {
        qint64 cur = view_->playheadFrame();
        if (cur < fbStartFrame_ || cur >= fbEndFrame_) cur = fbStartFrame_;
        startFfmpegFallback(cur, fbEndFrame_, fbLooping_);
        playBtn_->setText(playProc_ ? tr("Pause") : tr("Play"));
        if (!playProc_)
            statusBar()->showMessage(tr("No audio output available (check ALSA/PulseAudio/PipeWire or install ffmpeg)"), 5000);
        return;
    }

    qint64 start = view_->cursorFrame();
    qint64 end = audio_.totalFrames;
    if (view_->hasSelection()) {
        start = view_->selStart();
        end = view_->selEnd();
    }
    const bool loop = loopBtn_->isChecked();
    DBG(QStringLiteral("playPause: file=%1 frames=%2 rate=%3 ch=%4")
        .arg(currentPath_).arg(audio_.totalFrames).arg(audio_.sampleRate).arg(audio_.channels));

    // Preferred path: in-process playback via Qt Multimedia (QAudioSink with
    // an ALSA/PulseAudio/PipeWire device, or the aplay/ffplay pipe fallback).
    // If the sink dies right away (e.g. no plugin, device busy) we detect it
    // after a short delay and transparently switch to the direct ffmpeg path
    // so the button never just "blinks".
    DBG(QStringLiteral("playPause: trying Qt sink, start=%1 end=%2 loop=%3 sel=%4")
        .arg(start).arg(end).arg(loop).arg(view_->hasSelection()));
    player_.play(start, end, loop);
    if (player_.isPlaying()) {
        playBtn_->setText(tr("Pause"));
        QTimer::singleShot(250, this, [this, start, end, loop] {
            // Still running normally — nothing to do.
            if (player_.isPlaying() || playProc_) return;
            DBG("Qt audio output failed instantly, switching to ffmpeg fallback");
            startFfmpegFallback(start, end, loop);
            if (!playProc_)
                statusBar()->showMessage(tr("No audio output available (check ALSA/PulseAudio/PipeWire or install ffmpeg)"), 5000);
        });
        return;
    }
    // Sink unavailable outright — go straight to the direct ffmpeg fallback.
    startFfmpegFallback(start, end, loop);
    if (playProc_) {
        playBtn_->setText(tr("Pause"));
    } else {
        DBG("ffmpeg fallback did not start — no audio output available");
        statusBar()->showMessage(tr("No audio output available (check ALSA/PulseAudio/PipeWire or install ffmpeg)"), 5000);
        playBtn_->setText(tr("Play"));
    }
}

void MainWindow::stopPlayback() {
    player_.stop();
    stopFfmpegFallback();
    fbStartFrame_ = 0; fbEndFrame_ = 0;   // clear fallback resume state
    playBtn_->setText(tr("Play"));
    view_->setPlayheadFrame(player_.currentFrame());
}

// Seek to a frame while playback is active, in whichever output path runs.
// Called from the waveform click handler (AudioView::cursorMoved).
void MainWindow::seekPlaybackTo(qint64 frame) {
    if (!audio_.valid) return;
    frame = std::clamp(frame, qint64(0), audio_.totalFrames - 1);

    // In-process playback (Qt QAudioSink pull mode or pipe push-mode pump):
    // jump the position and keep playing/paused state as it is.
    if (player_.isPlaying() || player_.isPaused()) {
        player_.seek(frame);
        view_->setPlayheadFrame(frame);
        return;
    }

    // External ffmpeg fallback process: restart it from the new position so
    // playback continues there. The range end and loop flag are preserved.
    if (playProc_) {
        const bool loop = fbLooping_;
        const qint64 end = fbEndFrame_;
        stopFfmpegFallback();             // kills the proc, clears resume state
        startFfmpegFallback(frame, end, loop);
        if (playProc_) {
            playBtn_->setText(tr("Pause"));
            view_->setPlayheadFrame(frame);
        } else {
            playBtn_->setText(tr("Play"));
            statusBar()->showMessage(tr("Seek failed (external player could not restart)"), 4000);
        }
        return;
    }
}

// Keep the Play/Pause button in sync with the Qt sink state. The sink can
// finish on its own (end of file, device error) while we believe it is
// playing; without this the button would stay stuck on "Pause".
void MainWindow::updatePlayButton() {
    if (!playBtn_) return;
    const bool running = player_.isPlaying() || (playProc_ && wasPlaying_);
    playBtn_->setText(running ? tr("Pause") : tr("Play"));
}

// ---------------- ffmpeg direct-playback fallback ----------------
//
// On Ubuntu the Qt Multimedia stack may open a device (the ALSA/Pulse click
// you hear) yet fail to actually stream our format, so QAudioSink stops
// instantly. This fallback spawns an external player on a temp WAV copy of
// the decoded buffer — the same path VLC uses successfully.

void MainWindow::startFfmpegFallback(qint64 startFrame, qint64 endFrame, bool loop) {
    stopFfmpegFallback();
    if (!audio_.valid || audio_.sampleRate <= 0) return;
    fbStartFrame_ = startFrame;
    fbEndFrame_ = endFrame;
    fbLooping_ = loop;

    const double sr = audio_.sampleRate;
    // Resume from the current playhead if it was moved past the start point.
    qint64 fromFrame = std::max(startFrame, view_->playheadFrame());
    if (fromFrame >= endFrame) fromFrame = startFrame;   // finished range -> restart
    const double startSec = double(fromFrame) / sr;
    const double durSec = double(endFrame - fromFrame) / sr;
    if (durSec <= 0.001) return;

    QString ff = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ff.isEmpty()) {
        DBG("ffmpeg executable not found (QStandardPaths::findExecutable failed) — cannot use playback fallback");
        fbStartFrame_ = 0; fbEndFrame_ = 0;
        return;
    }

    // For uncompressed formats on disk (WAV/AIFF/CAF) feed ffmpeg the ORIGINAL
    // file directly: no temp copy, nothing written to disk. This matches what
    // VLC does. Compressed sources (decoded into memory via ffmpeg) still need
    // a temp WAV because the in-memory buffer may contain unsaved edits.
    const bool directFile = !currentPath_.isEmpty() && QFile::exists(currentPath_)
                            && isEditableFormat(currentPath_.toLower());
    QString inputPath;
    if (directFile) {
        inputPath = currentPath_;
    } else {
        playTmpPath_ = QDir(QDir::tempPath())
                          .filePath(QStringLiteral("audioscout_play_XXXXXX.wav"));
        QTemporaryFile tmp(playTmpPath_);
        tmp.setAutoRemove(false);
        if (!tmp.open()) { fbStartFrame_ = 0; fbEndFrame_ = 0; return; }
        playTmpPath_ = tmp.fileName();
        tmp.close();

        AudioData copy = audio_;
        QString err;
        if (!AudioFile::save(playTmpPath_, copy, &err)) {
            DBG(QStringLiteral("fallback temp WAV save failed: %1").arg(err));
            QFile::remove(playTmpPath_);
            playTmpPath_.clear();
            fbStartFrame_ = 0; fbEndFrame_ = 0;
            return;
        }
        inputPath = playTmpPath_;
    }

    playProc_ = new QProcess(this);
    connect(playProc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                if (playProc_) { playProc_->deleteLater(); playProc_ = nullptr; }
                if (fbPlayheadTimer_) fbPlayheadTimer_->stop();
                QFile::remove(playTmpPath_);
                playTmpPath_.clear();
                if (!wasPlaying_) return;      // stopped/paused intentionally
                wasPlaying_ = false;
                DBG(QStringLiteral("external player finished: exitCode=%1 status=%2")
                    .arg(code).arg(status == QProcess::CrashExit ? "CrashExit" : "NormalExit"));
                if (status == QProcess::CrashExit || code != 0) {
                    statusBar()->showMessage(tr("Playback failed (external player error)"), 5000);
                } else if (fbLooping_) {
                    // Loop: restart the range when the clip ends.
                    QTimer::singleShot(30, this, [this] {
                        if (wasPlaying_) return;
                        view_->setPlayheadFrame(fbStartFrame_);
                        startFfmpegFallback(fbStartFrame_, fbEndFrame_, true);
                    });
                    return;
                }
                if (playBtn_) playBtn_->setText(tr("Play"));
            });
    // ffmpeg writes directly to the ALSA "default" device, which resolves to
    // whatever output is configured in the system sound settings — no need to
    // enumerate /proc/asound/cards or pick a card name manually.
    QStringList args;
    args << "-hide_banner" << "-nostdin" << "-loglevel" << "warning" << "-re";
    if (startSec > 0) args << "-ss" << QString::number(startSec, 'f', 3);
    args << "-t" << QString::number(durSec, 'f', 3);
    args << "-i" << inputPath << "-vn" << "-acodec" << "pcm_s16le"
         << "-f" << "alsa" << "default";
    DBG(QStringLiteral("spawning: %1 %2").arg(ff, args.join(' ')));
    playProc_->setProgram(ff);
    playProc_->setArguments(args);
    // Capture ffmpeg stderr so ALSA errors end up in the debug log too.
    playProc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(playProc_, &QProcess::readyReadStandardError, this, [this] {
        const QString s = QString::fromUtf8(playProc_->readAllStandardError()).trimmed();
        if (!s.isEmpty()) {
            DBG(QStringLiteral("ffmpeg stderr: %1").arg(s));
            qWarning().noquote() << "[audioscout][ffmpeg]" << s;
        }
    });
    wasPlaying_ = true;
    fbElapsedMs_ = 0;
    fbFromFrame_ = fromFrame;
    fbClock_.start();

    // Advance the playhead in sync with real time while ffmpeg plays: the
    // external process gives us no position feedback, so we track wall clock.
    if (!fbPlayheadTimer_) {
        fbPlayheadTimer_ = new QTimer(this);
        fbPlayheadTimer_->setInterval(50);
        connect(fbPlayheadTimer_, &QTimer::timeout, this, [this] {
            if (!playProc_ || !wasPlaying_ || audio_.sampleRate <= 0) return;
            const double sr = audio_.sampleRate;
            qint64 elapsed = fbElapsedMs_ + (fbClock_.isValid() ? fbClock_.elapsed() : 0);
            qint64 f = fbFromFrame_ + qint64(elapsed * sr / 1000.0);
            if (f >= fbEndFrame_) {
                // Process is about to finish on its own; clamp at range end.
                view_->setPlayheadFrame(fbEndFrame_);
                fbPlayheadTimer_->stop();
                return;
            }
            view_->setPlayheadFrame(f);
        });
    }
    fbPlayheadTimer_->start();

    playProc_->start();
    if (!playProc_->waitForStarted(3000)) {
        DBG("failed to start ffmpeg playback process (waitForStarted timed out)");
        if (fbPlayheadTimer_) fbPlayheadTimer_->stop();
        playProc_->disconnect(this);
        playProc_->deleteLater();
        playProc_ = nullptr;
        wasPlaying_ = false;
        QFile::remove(playTmpPath_);
        playTmpPath_.clear();
        fbStartFrame_ = 0; fbEndFrame_ = 0;
    }
}

void MainWindow::resumeFfmpegFallback() {
    startFfmpegFallback(fbStartFrame_, fbEndFrame_, fbLooping_);
    if (playProc_) playBtn_->setText(tr("Pause"));
}

void MainWindow::stopFfmpegFallback() {
    wasPlaying_ = false;
    if (fbPlayheadTimer_) fbPlayheadTimer_->stop();
    if (!playProc_) return;
    fbElapsedMs_ += fbClock_.isValid() ? fbClock_.elapsed() : 0;
    // Remember roughly where playback stopped so the next Play resumes there.
    qint64 cur = view_->playheadFrame();
    if (cur < fbStartFrame_ || cur >= fbEndFrame_) {
        const double sr = audio_.sampleRate > 0 ? audio_.sampleRate : 1;
        const double elapsed = fbElapsedMs_ / 1000.0;
        cur = std::min(fbEndFrame_, fbStartFrame_ + qint64(elapsed * sr));
    }
    view_->setPlayheadFrame(cur);
    playProc_->disconnect(this);
    if (playProc_->state() != QProcess::NotRunning) {
        playProc_->kill();
        playProc_->waitForFinished(500);
    }
    playProc_->deleteLater();
    playProc_ = nullptr;
    if (!playTmpPath_.isEmpty()) {
        QFile::remove(playTmpPath_);
        playTmpPath_.clear();
    }
}

void MainWindow::prevFile() {
    int row = currentRow();
    if (row > 0) {
        if (player_.isPlaying() || playProc_) {
            player_.stop();
            stopFfmpegFallback();
            playBtn_->setText(tr("Play"));
        }
        table_->selectRow(row - 1);
        table_->scrollTo(model_->index(row - 1, 0));
        loadCurrentFile();   // prev/next load the file immediately
    }
}

void MainWindow::nextFile() {
    int row = currentRow();
    if (row >= 0 && row + 1 < model_->rows().size()) {
        if (player_.isPlaying() || playProc_) {
            player_.stop();
            stopFfmpegFallback();
            playBtn_->setText(tr("Play"));
        }
        table_->selectRow(row + 1);
        table_->scrollTo(model_->index(row + 1, 0));
        loadCurrentFile();   // prev/next load the file immediately
    } else {
        stopPlayback();   // end of list -> stop (spec 2.6)
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
    op();
    undoStack_.push(rec);
    undoStack_.enforceLimits(Settings::instance().undoDepth(),
                             Settings::instance().undoDiskLimitBytes());
    if (!AudioFile::save(currentPath_, audio_, &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
    } else {
        persistMarkers();
        // file changed: invalidate its preview cache entry and re-request preview
        Database::instance().invalidatePreviewsForDir(currentDir_);
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
        double dbfs = normLevel_->value();
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
    if (idx < 0 || idx >= markers_.size()) return;
    view_->setActiveMarkerIndex(idx);
    // Double click on a marker opens the editor popup (spec 4.2).
    static qint64 lastClickMs = 0;
    static int lastIdx = -1;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool dbl = (idx == lastIdx && now - lastClickMs < QApplication::doubleClickInterval());
    lastClickMs = now; lastIdx = idx;
    if (!dbl) return;

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
    tableModeCombo_->setCurrentIndex(Settings::instance().tablePreviewMode() == QLatin1String("spectrogram") ? 1 : 0);
}
