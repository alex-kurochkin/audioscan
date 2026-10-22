#include "FfmpegFallbackController.h"

#include "AudioDebug.h"
#include "AudioFile.h"
#include "Settings.h"
#include "Types.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <algorithm>

// The ffmpeg direct-playback fallback.
//
// On Ubuntu the Qt Multimedia stack may open a device (the ALSA/Pulse click
// you hear) yet fail to actually stream our format, so QAudioSink stops
// instantly. This fallback spawns an external player on a temp WAV copy of
// the decoded buffer — the same path VLC uses successfully.

FfmpegFallbackController::FfmpegFallbackController(QObject *parent)
    : QObject(parent) {}

FfmpegFallbackController::~FfmpegFallbackController() { stop(); }

bool FfmpegFallbackController::start(qint64 startFrame, qint64 endFrame, bool loop) {
    stop();   // one session at a time; keeps nothing of the previous process
    const AudioData *audio = this->audio();
    if (!audio || !audio->valid || audio->sampleRate <= 0) return false;
    paused_ = false;   // a live session exists again
    startFrame_ = startFrame;
    endFrame_ = endFrame;
    looping_ = loop;

    const double sr = audio->sampleRate;
    // Resume from the current playhead if it was moved past the start point.
    qint64 fromFrame = std::max(startFrame, playhead());
    if (fromFrame >= endFrame) fromFrame = startFrame;   // finished range -> restart
    const double startSec = double(fromFrame) / sr;
    const double durSec = double(endFrame - fromFrame) / sr;
    if (durSec <= 0.001) return false;

    QString ff = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ff.isEmpty()) {
        DBG("ffmpeg executable not found (QStandardPaths::findExecutable failed) — cannot use playback fallback");
        startFrame_ = 0; endFrame_ = 0;
        return false;
    }

    // For uncompressed formats on disk (WAV/AIFF/CAF) feed ffmpeg the ORIGINAL
    // file directly: no temp copy, nothing written to disk. This matches what
    // VLC does. Compressed sources (decoded into memory via ffmpeg) still need
    // a temp WAV because the in-memory buffer may contain unsaved edits.
    const QString curPath = currentPath();
    const bool directFile = !curPath.isEmpty() && QFile::exists(curPath)
                            && isEditableFormat(curPath.toLower());
    QString inputPath;
    if (directFile) {
        inputPath = curPath;
    } else {
        tmpPath_ = QDir(QDir::tempPath())
                          .filePath(QStringLiteral("audioscout_play_XXXXXX.wav"));
        QTemporaryFile tmp(tmpPath_);
        tmp.setAutoRemove(false);
        if (!tmp.open()) { startFrame_ = 0; endFrame_ = 0; return false; }
        tmpPath_ = tmp.fileName();
        tmp.close();

        AudioData copy = *audio;
        QString err;
        if (!AudioFile::save(tmpPath_, copy, &err)) {
            DBG(QStringLiteral("fallback temp WAV save failed: %1").arg(err));
            QFile::remove(tmpPath_);
            tmpPath_.clear();
            startFrame_ = 0; endFrame_ = 0;
            return false;
        }
        inputPath = tmpPath_;
    }

    proc_ = new QProcess(this);
    connect(proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                if (proc_) { proc_->deleteLater(); proc_ = nullptr; }
                if (playheadTimer_) playheadTimer_->stop();
                QFile::remove(tmpPath_);
                tmpPath_.clear();
                if (!wasPlaying_) return;      // stopped/paused intentionally
                wasPlaying_ = false;
                DBG(QStringLiteral("external player finished: exitCode=%1 status=%2")
                    .arg(code).arg(status == QProcess::CrashExit ? "CrashExit" : "NormalExit"));
                if (status == QProcess::CrashExit || code != 0) {
                    if (onFinished_) onFinished_(true);
                    return;
                }
                if (looping_ && loopChecked()) {
                    // Loop: restart the range when the clip ends. The button
                    // check is re-read at this moment, so un-checking Loop
                    // while the tail plays stops the session for good instead
                    // of restarting it one more time.
                    QTimer::singleShot(30, this, [this] {
                        if (wasPlaying_) return;
                        setPlayhead(startFrame_);
                        start(startFrame_, endFrame_, true);
                    });
                    return;
                }
                if (onFinished_) onFinished_(false);
            });
    // ffmpeg writes directly to the ALSA "default" device, which resolves to
    // whatever output is configured in the system sound settings — no need to
    // enumerate /proc/asound/cards or pick a card name manually.
    QStringList args;
    args << "-hide_banner" << "-nostdin" << "-loglevel" << "error" << "-re";
    if (startSec > 0) args << "-ss" << QString::number(startSec, 'f', 3);
    args << "-t" << QString::number(durSec, 'f', 3);
    args << "-i" << inputPath << "-vn" << "-acodec" << "pcm_s16le"
         << "-f" << "alsa" << "default";
    DBG(QStringLiteral("spawning: %1 %2").arg(ff, args.join(' ')));
    proc_->setProgram(ff);
    proc_->setArguments(args);
    // Capture ffmpeg stderr so ALSA errors end up in the debug log too.
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardError, this, [this] {
        const QString s = QString::fromUtf8(proc_->readAllStandardError()).trimmed();
        if (!s.isEmpty()) {
            // Keep it in the opt-in debug log only; don't spam the terminal.
            DBG(QStringLiteral("ffmpeg stderr: %1").arg(s));
        }
    });
    wasPlaying_ = true;
    elapsedMs_ = 0;
    fromFrame_ = fromFrame;
    clock_.start();

    // Advance the playhead in sync with real time while ffmpeg plays: the
    // external process gives us no position feedback, so we track wall clock.
    if (!playheadTimer_) {
        playheadTimer_ = new QTimer(this);
        playheadTimer_->setInterval(50);
        connect(playheadTimer_, &QTimer::timeout, this, [this] {
            const AudioData *a = this->audio();
            if (!proc_ || !wasPlaying_ || !a || a->sampleRate <= 0) return;
            const double sr = a->sampleRate;
            qint64 elapsed = elapsedMs_ + (clock_.isValid() ? clock_.elapsed() : 0);
            qint64 f = fromFrame_ + qint64(elapsed * sr / 1000.0);
            if (f >= endFrame_) {
                // Process is about to finish on its own; clamp at range end.
                setPlayhead(endFrame_);
                playheadTimer_->stop();
                return;
            }
            setPlayhead(f);
        });
    }
    playheadTimer_->start();

    proc_->start();
    if (!proc_->waitForStarted(3000)) {
        DBG("failed to start ffmpeg playback process (waitForStarted timed out)");
        if (playheadTimer_) playheadTimer_->stop();
        proc_->disconnect(this);
        proc_->deleteLater();
        proc_ = nullptr;
        wasPlaying_ = false;
        QFile::remove(tmpPath_);
        tmpPath_.clear();
        startFrame_ = 0; endFrame_ = 0;
        return false;
    }
    return true;
}

void FfmpegFallbackController::stop() {
    wasPlaying_ = false;
    if (playheadTimer_) playheadTimer_->stop();
    if (!proc_) return;
    elapsedMs_ += clock_.isValid() ? clock_.elapsed() : 0;
    // Remember roughly where playback stopped so the next Play resumes there.
    qint64 cur = playhead();
    if (cur < startFrame_ || cur >= endFrame_) {
        const AudioData *a = audio();
        const double sr = (a && a->sampleRate > 0) ? a->sampleRate : 1;
        const double elapsed = elapsedMs_ / 1000.0;
        cur = std::min(endFrame_, startFrame_ + qint64(elapsed * sr));
    }
    setPlayhead(cur);
    proc_->disconnect(this);
    if (proc_->state() != QProcess::NotRunning) {
        proc_->kill();
        proc_->waitForFinished(500);
    }
    proc_->deleteLater();
    proc_ = nullptr;
    if (!tmpPath_.isEmpty()) {
        QFile::remove(tmpPath_);
        tmpPath_.clear();
    }
}

void FfmpegFallbackController::resetState() {
    stop();
    startFrame_ = 0;
    endFrame_ = 0;
    paused_ = false;
}

// Restart the running ffmpeg session at the current playhead position with
// the freshly stored volume. ffmpeg has no live gain control, so the only way
// to make the volume slider affect this output path is to relaunch the
// process from where playback currently is, preserving range end and loop.
bool FfmpegFallbackController::restartWithVolume() {
    if (!proc_ || !wasPlaying_) return false;
    const bool loop = looping_;
    const qint64 end = endFrame_;
    qint64 cur = playhead();
    if (cur < startFrame_ || cur >= end) cur = startFrame_;
    stop();                       // kills proc, keeps range bookkeeping
    return start(cur, end, loop);
}
