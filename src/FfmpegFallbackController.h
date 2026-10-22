#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <functional>

struct AudioData;

// Direct ffmpeg fallback: when neither Qt Multimedia nor the in-process pipe
// sinks can produce sound, we spawn "ffmpeg -re -i <file>" so that playback
// always works. This controller owns the external process, its temp WAV copy
// (for decoded/compressed sources with unsaved edits), and the wall-clock
// playhead estimation — the external player gives no position feedback.
//
// All interaction with the rest of the UI goes through injected callbacks,
// so the class is independent of MainWindow:
//   playhead()          — current visible cursor frame
//   setPlayhead(frame)  — move the visible cursor (caller handles the
//                         programmatic-move re-entrancy guard itself)
//   loopChecked()       — is the Loop button currently armed
//   onFinished(err)     — the session ended on its own (natural end or error)
class FfmpegFallbackController : public QObject {
    Q_OBJECT
public:
    explicit FfmpegFallbackController(QObject *parent = nullptr);
    ~FfmpegFallbackController() override;

    void setProviders(const AudioData *(*audio)(),
                      qint64 (*playhead)(),
                      void (*setPlayhead)(qint64),
                      bool (*loopChecked)()) {
        audioFn_ = audio;
        playheadFn_ = playhead;
        setPlayheadFn_ = setPlayhead;
        loopCheckedFn_ = loopChecked;
    }
    void setOnFinished(std::function<void(bool error)> cb) {
        onFinished_ = std::move(cb);
    }
    // The file currently loaded in the player (decides direct-file vs temp).
    void setCurrentPath(const QString &p) { currentPath_ = p; }

    bool isActive() const { return proc_ != nullptr; }
    bool wasPlaying() const { return wasPlaying_; }
    bool isPausedSession() const { return paused_; }
    void setPausedSession(bool p) { paused_ = p; }
    qint64 startFrame() const { return startFrame_; }
    qint64 endFrame() const { return endFrame_; }
    bool looping() const { return looping_; }
    // Range bookkeeping for callers that retarget a live session in place
    // (loop toggling, seek): set without respawning the process.
    void setRange(qint64 startFrame, qint64 endFrame, bool loop) {
        startFrame_ = startFrame; endFrame_ = endFrame; looping_ = loop;
    }

    // Spawn playback of [startFrame, endFrame) of the current buffer.
    // Returns true if the process started. Resumes from the playhead when
    // it sits inside the range.
    bool start(qint64 startFrame, qint64 endFrame, bool loop);
    // Kill the process; remembers the elapsed position for a later resume
    // and moves the playhead to the stopped position. Keeps start/end/loop
    // bookkeeping intact (pause semantics of the original code).
    void stop();
    // stop() + drop all resume bookkeeping (explicit Stop / superseded state).
    void resetState();
    // Relaunch at the current playhead with the freshly stored volume
    // (ffmpeg reads gain only at startup). Preserves range end and loop.
    // Returns true when the relaunched session is running.
    bool restartWithVolume();

private:
    const AudioData *(*audioFn_)() = nullptr;
    qint64 (*playheadFn_)() = nullptr;
    void (*setPlayheadFn_)(qint64) = nullptr;
    bool (*loopCheckedFn_)() = nullptr;
    std::function<void(bool error)> onFinished_;

    const AudioData *audio() const { return audioFn_ ? audioFn_() : nullptr; }
    qint64 playhead() const { return playheadFn_ ? playheadFn_() : 0; }
    void setPlayhead(qint64 f) { if (setPlayheadFn_) setPlayheadFn_(f); }

    QProcess *proc_ = nullptr;
    QTimer *playheadTimer_ = nullptr;
    QElapsedTimer clock_;
    QString tmpPath_;
    QString currentPath_;
    qint64 startFrame_ = 0, endFrame_ = 0;
    qint64 fromFrame_ = 0;            // frame playback started from (this proc)
    qint64 elapsedMs_ = 0;            // wall-clock ms the proc ran (accumulated)
    bool looping_ = false;
    bool wasPlaying_ = false;         // session active (vs. stopped/paused)
    bool paused_ = false;             // paused session kept for resume
};
