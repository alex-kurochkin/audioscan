#include "PlayerController.h"

#include "AudioDebug.h"
#include "AudioFile.h"
#include "AudioView.h"
#include "Types.h"

#include <QPushButton>
#include <QTimer>

namespace {
const char *kNoOutputMsg =
    "No audio output available (check ALSA/PulseAudio/PipeWire or install ffmpeg)";
}  // namespace

PlayerController::PlayerController(AudioPlayer *player,
                                   FfmpegFallbackController *fb,
                                   PlayerUi *ui, QObject *parent)
    : QObject(parent), player_(player), fb_(fb), ui_(ui) {}

bool PlayerController::isRunning() const {
    if (!player_) return fallbackActive();
    return player_->isPlaying() || player_->isPaused() || fallbackActive();
}

bool PlayerController::startFallback(qint64 startFrame, qint64 endFrame, bool loop) {
    if (!fb_) return false;
    // Programmatic session: the cursor moves inside must never be read as a
    // user click-to-seek.
    const bool ok = fb_->start(startFrame, endFrame, loop);
    if (ok && ui_) ui_->updatePlayButton();
    return ok;
}

void PlayerController::stopFallback() { if (fb_) fb_->stop(); }
void PlayerController::resetFallbackState() { if (fb_) fb_->resetState(); }

void PlayerController::playPause() {
    playPauseInner();
    // Single source of truth for the button label after every start/pause/
    // resume path: whatever actually ended up running decides the text, so
    // the button can never stay on "Play" while audio is playing (the old
    // "blinks and stays Play" bug) nor on "Pause" after a failed start.
    if (ui_) ui_->updatePlayButton();
}

void PlayerController::playPauseInner() {
    // Every pointer this path touches may still be unwired early in the
    // window's lifetime (setContext()/setFallback() run late in buildUi()).
    // Bail out instead of crashing — a stray Space press must not SIGSEGV.
    if (!player_ || !audio_ || !audio_->valid || !view_ || !loopBtn_ || !playBtn_ || !ui_) return;
    if (player_->isPlaying()) {          // in-process Qt sink is running
        player_->pause();
        playBtn_->setText(QObject::tr("Play"));
        return;
    }
    if (player_->isPaused()) {           // resume Qt playback where we left off
        player_->resume();
        playBtn_->setText(QObject::tr("Pause"));
        return;
    }
    if (fallbackActive()) {              // external fallback is running: pause it
        qint64 cur = ui_->playhead();
        fb_->stop();                     // keeps start/end/loop, resets wasPlaying_
        if (cur < fb_->startFrame() || cur >= fb_->endFrame())
            cur = fb_->startFrame();
        ui_->setPlayhead(cur, true);     // resume point remembered by -ss below
        fb_->setPausedSession(true);     // mark the session as paused-resumable
        playBtn_->setText(QObject::tr("Play"));
        return;
    }
    // Paused ffmpeg fallback: resume from the playhead position.
    if (fb_ && fb_->isPausedSession() &&
        (fb_->startFrame() > 0 || fb_->endFrame() > 0)) {
        qint64 cur = ui_->playhead();
        if (cur < fb_->startFrame() || cur >= fb_->endFrame())
            cur = fb_->startFrame();
        fb_->setPausedSession(false);
        const bool started = startFallback(cur, fb_->endFrame(), fb_->looping());
        playBtn_->setText(started ? QObject::tr("Pause") : QObject::tr("Play"));
        if (!started)
            ui_->showStatus(QObject::tr(kNoOutputMsg), 5000);
        return;
    }

    // After Stop: resume from where playback last started, not from the
    // cursor/playhead. The user can jump to a new spot by clicking the
    // waveform, or press W to go to the beginning of the file.
    // Loop never invents its own repeat start: when armed, the session range
    // is re-derived exactly like a fresh Play does — selection if any,
    // otherwise the whole file.
    // A selection always owns the playback range: with one present, every
    // start/resume path below re-derives [selStart, selEnd) instead of using
    // stale resume points. Without a selection the range is the whole file
    // and playback begins at the visible cursor.
    const bool loop = loopBtn_->isChecked();
    const bool hasSel = view_->hasSelection();
    const qint64 selStart = hasSel ? view_->selStart() : 0;
    const qint64 selEnd   = hasSel ? view_->selEnd()   : audio_->totalFrames;

    if (lastPlayStartFrame_ >= 0) {
        qint64 start = lastPlayStartFrame_;
        qint64 end   = audio_->totalFrames;
        if (hasSel) {
            start = selStart;         // selection wins over any old resume point
            end   = selEnd;
        } else if (loop) {
            start = 0;                // loop without selection repeats the file
        }
        // Resume from the visible cursor when it sits inside the resume range
        // (e.g. after a live seek during the previous session); otherwise the
        // session simply starts at its recorded start point. The cursor and
        // the audible position must never disagree.
        const qint64 ph = ui_->playhead();
        if (!hasSel && ph > start && ph < end) start = ph;
        player_->play(start, end, loop);
        if (player_->isPlaying()) {
            lastPlayStartFrame_ = start;
            playBtn_->setText(QObject::tr("Pause"));
            QTimer::singleShot(250, this, [this, start, end, loop] {
                if (player_->isPlaying() || fallbackActive()) return;
                DBG("Qt audio output failed instantly (resume), switching to ffmpeg fallback");
                if (startFallback(start, end, loop)) {
                    lastPlayStartFrame_ = start;
                    playBtn_->setText(QObject::tr("Pause"));
                } else {
                    playBtn_->setText(QObject::tr("Play"));
                    ui_->showStatus(QObject::tr(kNoOutputMsg), 5000);
                }
            });
            return;
        }
        if (startFallback(start, end, loop)) {
            lastPlayStartFrame_ = start;
            playBtn_->setText(QObject::tr("Pause"));
            return;
        }
        playBtn_->setText(QObject::tr("Play"));
        lastPlayStartFrame_ = -1;   // no working output — fall through to normal start
    }

    qint64 start = hasSel ? selStart : view_->cursorFrame();
    qint64 end = hasSel ? selEnd : audio_->totalFrames;
    DBG(QStringLiteral("playPause: frames=%1 rate=%2 ch=%3")
        .arg(audio_->totalFrames).arg(audio_->sampleRate).arg(audio_->channels));

    // Preferred path: in-process playback via Qt Multimedia (QAudioSink with
    // an ALSA/PulseAudio/PipeWire device, or the aplay/ffplay pipe fallback).
    // If the sink dies right away (e.g. no plugin, device busy) we detect it
    // after a short delay and transparently switch to the direct ffmpeg path
    // so the button never just "blinks".
    DBG(QStringLiteral("playPause: trying Qt sink, start=%1 end=%2 loop=%3 sel=%4")
        .arg(start).arg(end).arg(loop).arg(view_->hasSelection()));
    player_->play(start, end, loop);
    lastPlayStartFrame_ = start;   // remember where this session began (Stop/Play resume point)
    if (player_->isPlaying()) {
        playBtn_->setText(QObject::tr("Pause"));
        QTimer::singleShot(250, this, [this, start, end, loop] {
            // Still running normally — nothing to do.
            if (player_->isPlaying() || fallbackActive()) return;
            DBG("Qt audio output failed instantly, switching to ffmpeg fallback");
            if (!startFallback(start, end, loop))
                ui_->showStatus(QObject::tr(kNoOutputMsg), 5000);
        });
        return;
    }
    // Sink unavailable outright — go straight to the direct ffmpeg fallback.
    if (startFallback(start, end, loop)) {
        playBtn_->setText(QObject::tr("Pause"));
    } else {
        DBG("ffmpeg fallback did not start — no audio output available");
        ui_->showStatus(QObject::tr(kNoOutputMsg), 5000);
        playBtn_->setText(QObject::tr("Play"));
    }
}

void PlayerController::stopPlayback() {
    if (!player_ || !ui_) return;   // unwired controller: nothing to stop
    // Qt session: Stop rewinds the cursor to where playback last started.
    const qint64 qtResume = player_->isPlaying() || player_->isPaused()
                                ? player_->startFrame() : -1;
    // ffmpeg fallback: same rule — return to the session's start point.
    const bool fbActive = fallbackActive() && fb_->wasPlaying();
    const qint64 fbResume = fbActive ? fb_->startFrame() : -1;

    player_->stop();
    resetFallbackState();
    if (playBtn_) playBtn_->setText(QObject::tr("Play"));
    if (qtResume >= 0)
        ui_->setPlayhead(qtResume, true);
    else if (fbResume >= 0)
        ui_->setPlayhead(fbResume, true);
    else
        ui_->setPlayhead(player_->currentFrame(), true);
}

// Seek to a frame while playback is active, in whichever output path runs.
// Called from the waveform click handler (AudioView::cursorMoved).
void PlayerController::seekPlaybackTo(qint64 frame) {
    if (!player_ || !audio_ || !audio_->valid || !view_ || !ui_) return;

    // In-process playback (Qt QAudioSink pull mode or pipe pump): jump the
    // position and keep playing/paused state as it is. The seek stays inside
    // the session range — clicking past the end of a selection does NOT
    // extend it: audio runs on from the click to the selection end and stops
    // there, honouring the selection boundary.
    if (player_->isPlaying() || player_->isPaused()) {
        player_->seek(frame);
        ui_->setPlayhead(player_->currentFrame(), true);
        return;
    }

    // External ffmpeg fallback process: restart it from the new position so
    // playback continues there. A click inside the current range keeps the
    // range (and loop) intact. A click *outside* it retargets only the
    // *start* to the clicked frame; the end follows the selection rule —
    // with a live selection playback still stops at the selection boundary,
    // without one it runs to the end of the file.
    if (fb_ && (fb_->isActive() || fb_->startFrame() > 0 || fb_->endFrame() > 0)) {
        const qint64 prevStart = fb_->startFrame(), prevEnd = fb_->endFrame();
        const bool prevLoop = fb_->looping();
        const bool wasPausedSession = !fb_->isActive();   // kept for resume
        const bool hasSel = view_->hasSelection();
        qint64 end = hasSel ? view_->selEnd() : audio_->totalFrames;
        if (end <= frame) end = audio_->totalFrames; // clicked past the selection end
        fb_->stop();                       // kills the proc, keeps bookkeeping
        if (startFallback(frame, end, prevLoop)) {
            playBtn_->setText(QObject::tr("Pause"));
            ui_->setPlayhead(frame, true);
            lastPlayStartFrame_ = std::min(prevStart, frame); // Stop rewinds here
        } else {
            // Restart failed: restore the previous session state so Stop/Play
            // bookkeeping stays consistent, and report.
            fb_->setRange(prevStart, prevEnd, prevLoop);
            fb_->setPausedSession(wasPausedSession);
            playBtn_->setText(QObject::tr("Play"));
            ui_->showStatus(QObject::tr("Seek failed (external player could not restart)"), 4000);
        }
        return;
    }
}

// Loop button toggled: works while stopped AND while playing. Loop never
// sets up its own repeat start point: the looped range is exactly what Play
// would use — the current selection if there is one, otherwise the whole
// file. While playback is active the range is re-armed immediately so
// looping takes effect live.
void PlayerController::onLoopToggled(bool on) {
    if (!player_ || !audio_ || !audio_->valid || !view_ || !ui_) return;
    const qint64 selStart = view_->selStart();
    const qint64 selEnd   = view_->selEnd();
    const bool hasSel     = view_->hasSelection();
    const qint64 rangeStart = hasSel ? selStart : 0;
    const qint64 rangeEnd   = hasSel ? selEnd : audio_->totalFrames;

    if (player_->isPlaying() || player_->isPaused()) {
        // Loop never invents its own repeat start: the repeat range is
        // exactly what the session plays — selection if any, otherwise
        // the whole file. Retarget only the loop boundary in place; do
        // NOT restart the output (that used to drop a chunk of already
        // buffered audio and re-open the device — audible glitch) and do
        // NOT touch the play-start point (Stop must still rewind there).
        const qint64 cur = ui_->playhead();
        if (player_->setLoopRange(rangeStart, rangeEnd)) {
            player_->setLooping(on);
            ui_->rangeChanged(rangeStart, rangeEnd);
            if (cur < rangeStart || cur >= rangeEnd) {
                // Playhead outside the new repeat range: jump it back in.
                player_->seek(rangeStart);
                ui_->setPlayhead(player_->currentFrame(), true);
            }
        }
        ui_->updatePlayButton();   // keep the Loop highlight in sync right now
        return;                    // Qt sink loops internally from here on
    }
    if (fallbackActive()) {
        // ffmpeg fallback is running: restart it over the same range with
        // the new loop flag, resuming from the current playhead position.
        qint64 cur = ui_->playhead();
        if (cur < rangeStart || cur >= rangeEnd) cur = rangeStart;
        fb_->stop();                   // keeps session alive across restart
        if (startFallback(rangeStart, rangeEnd, on)) {
            playBtn_->setText(QObject::tr("Pause"));
            ui_->setPlayhead(cur, true);
        } else {
            playBtn_->setText(QObject::tr("Play"));
        }
    }
    // Stopped: nothing to restart — the next Play picks up the loop flag
    // and computes the same selection-or-whole-file range itself.
}

// Live volume change: the ffmpeg direct-playback fallback is a separate
// process with its own gain (read only at startup), so restart it at the
// current position with the new value when it is the active output path.
void PlayerController::onVolumeChanged() {
    if (fallbackActive() && fb_->wasPlaying()) {
        if (fb_->restartWithVolume()) {
            if (playBtn_) playBtn_->setText(QObject::tr("Pause"));
        } else {
            if (playBtn_) playBtn_->setText(QObject::tr("Play"));
            if (ui_) ui_->showStatus(QObject::tr("Volume change failed (external player could not restart)"), 4000);
        }
    }
}

// The fallback process ended on its own.
void PlayerController::onFallbackFinished(bool error) {
    if (error) {
        if (ui_) ui_->showStatus(QObject::tr("Playback failed (external player error)"), 5000);
        return;
    }
    if (playBtn_) playBtn_->setText(QObject::tr("Play"));
    if (ui_) ui_->updatePlayButton();   // fallback session over: sync Pause/Loop styling
}
