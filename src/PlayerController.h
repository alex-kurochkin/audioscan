#pragma once
#include <QObject>
#include <QString>

#include "AudioPlayer.h"
#include "FfmpegFallbackController.h"

// Owns the playback session state that used to bloat MainWindow: the Qt
// AudioPlayer plus the external ffmpeg fallback, the per-file resume point,
// and the play/pause/stop/seek/loop-toggle decision logic. Everything the
// UI must reflect goes through the PlayerUi interface below, so this class
// has no knowledge of widgets.
struct PlayerUi {
    virtual ~PlayerUi() = default;
    // Move the visible cursor. `programmatic` marks moves caused by the
    // engine itself (timer ticks, stop-rewind, seek echo) — the owner must
    // not interpret those as user click-to-seek (re-entrancy guard).
    virtual void setPlayhead(qint64 frame, bool programmatic) = 0;
    virtual qint64 playhead() const = 0;
    virtual void showStatus(const QString &msg, int msec) = 0;
    virtual void updatePlayButton() = 0;      // sync Play/Pause + Loop styling
    // Session range changed (loop toggle / seek): informational only.
    virtual void rangeChanged(qint64 startFrame, qint64 endFrame) = 0;
};

class AudioData;
class AudioView;
class QPushButton;
class QLabel;

// Playback orchestration: decides between the in-process Qt sink and the
// direct ffmpeg fallback, keeps the resume point, loop range and pause
// bookkeeping consistent across all paths.
class PlayerController : public QObject {
    Q_OBJECT
public:
    PlayerController(AudioPlayer *player, FfmpegFallbackController *fb,
                     PlayerUi *ui, QObject *parent = nullptr);

    void setContext(AudioData *audio, AudioView *view, QPushButton *loopBtn,
                    QPushButton *playBtn) {
        audio_ = audio; view_ = view; loopBtn_ = loopBtn; playBtn_ = playBtn;
    }

    // ---- entry points (wired to buttons/hotkeys by MainWindow) ----
    void playPause();          // start / pause / resume, then sync the button
    void stopPlayback();
    // Seek during playback: continues from the new position in whichever
    // output path is active. No-op when nothing is playing.
    void seekPlaybackTo(qint64 frame);
    // Loop button toggled: re-arms the live session over the current
    // selection-or-whole-file range without restarting the output.
    void onLoopToggled(bool on);
    // Volume slider moved while the ffmpeg fallback runs: relaunch it with
    // the new gain at the current position.
    void onVolumeChanged();
    // The fallback process ended on its own (natural end or error).
    void onFallbackFinished(bool error);

    // ---- session bookkeeping exposed to MainWindow ----
    bool fallbackActive() const { return fb_->isActive(); }
    bool fallbackWasPlaying() const { return fb_->wasPlaying(); }
    bool fallbackPausedSession() const { return fb_->isPausedSession(); }
    void setFallbackPausedSession(bool p) { fb_->setPausedSession(p); }
    qint64 fallbackStartFrame() const { return fb_->startFrame(); }
    qint64 fallbackEndFrame() const { return fb_->endFrame(); }
    bool fallbackLooping() const { return fb_->looping(); }
    void setFallbackRange(qint64 s, qint64 e, bool loop) { fb_->setRange(s, e, loop); }
    // Start the ffmpeg fallback directly (used by the deferred "Qt sink died
    // instantly" checks); returns true when the process is running.
    bool startFallback(qint64 startFrame, qint64 endFrame, bool loop);
    void stopFallback();       // pause semantics: keeps range for resume
    void resetFallbackState(); // explicit Stop: drops resume bookkeeping

    qint64 lastPlayStartFrame() const { return lastPlayStartFrame_; }
    void setLastPlayStartFrame(qint64 f) { lastPlayStartFrame_ = f; }
    // Any audible session running (Qt sink or fallback)?
    bool isRunning() const;

private:
    void playPauseInner();

    AudioPlayer *player_ = nullptr;         // not owned (owned by MainWindow)
    FfmpegFallbackController *fb_ = nullptr; // not owned
    PlayerUi *ui_ = nullptr;
    AudioData *audio_ = nullptr;
    AudioView *view_ = nullptr;
    QPushButton *loopBtn_ = nullptr;
    QPushButton *playBtn_ = nullptr;
    // Frame where the last Play session started. Stop keeps the playhead in
    // place; pressing Play again resumes from this point. -1 = no resume pt.
    qint64 lastPlayStartFrame_ = -1;
};
