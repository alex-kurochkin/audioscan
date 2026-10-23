#pragma once
#include <QAudioSink>
#include <QAudioFormat>
#include <QObject>
#include <QIODevice>
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <functional>
// Playback of an interleaved float PCM buffer via Qt Multimedia (QAudioSink).
// Supports volume gain, loop of a frame range, and start/end range playback.
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // Sink factory: creates the output device for a given format. The device
    // must already be opened WriteOnly and parented to this AudioPlayer; it is
    // passed directly to QAudioSink::start(). Defaults to nullptr (real
    // QAudioSink on the default audio device); can be replaced (e.g. with an
    // "aplay"/"ffplay" pipe fallback when Qt Multimedia has no usable device).
    using SinkFactory = std::function<QIODevice *(const QAudioFormat &)>;
    void setSinkFactory(SinkFactory f) { sinkFactory_ = std::move(f); }

    // Live volume changes for outputs whose gain is fixed at process spawn
    // time (the aplay/ffplay pipe fallbacks): recreate the output device via
    // the factory so it is spawned anew with the current volume, then resume
    // feeding from the unchanged playback position. No-op in Qt-sink pull
    // mode (volume is applied per-sample there). Called from the GUI thread.
    void restartOutputKeepingPosition();

    // Set the whole decoded buffer (interleaved float32).
    void setBuffer(const float *data, qint64 totalFrames, int channels, int sampleRate);

    void play(qint64 startFrame, qint64 endFrame, bool loop);  // end<=start => play to file end
    void pause();
    void resume();   // continue from the paused position (no rewind)
    void resumeOrStopToggle();
    void stop();     // stops and returns position to last play-start point
    // Jump to a new position while keeping playback state: if currently
    // playing or paused, output continues from frame (a paused session stays
    // paused at the new position). Outside an active session it only moves
    // the remembered position.
    void seek(qint64 frame);
    bool isPlaying() const { return playing_; }
    bool isPaused() const { return paused_; }

    // Volume is read by the audio callback thread, so it must be atomic.
    // setVolume also notifies the output owner (volumeApplier_) so devices
    // with startup-fixed gain (e.g. the "aplay" pipe fallback) can react.
    void setVolume(double v);
    double volume() const { return volume_.load(); }
    // Live volume changes must be applied to the already-created output
    // device too: aplay reads its own -volume argument at startup, so the
    // process has to be respawned with the new value.
    using VolumeApplier = std::function<void(double)>;
    void setVolumeApplier(VolumeApplier f) { volumeApplier_ = std::move(f); }

    // Toggle loop of the current playback range live (while playing).
    // Changing the flag mid-session emits loopStateChanged so the UI can
    // keep the Loop button highlight in sync with actual loop activity.
    void setLooping(bool on);
    bool isLooping() const { return looping_; }
    // Loop state that actually drives audible repetition right now: true only
    // while a session is active AND looping is armed. The UI uses this to
    // decide whether the Loop button should look highlighted.
    bool isLoopActive() const { return looping_ && (playing_ || paused_); }

    qint64 currentFrame() const { return posFrames_; }
    qint64 startFrame() const { return startFrames_; }
    // End of the active/last playback range (selection end or file end).
    qint64 endFrame() const { return loopEnd_; }

    // Live retargeting of the *repeat* boundary without restarting audio.
    // MainWindow uses this when Loop is toggled during a session: the loop
    // must repeat exactly the range the current session plays (selection if
    // any, otherwise the whole file) — never a boundary left over from an
    // earlier session or seek. Returns true if an active session exists.
    bool setLoopRange(qint64 start, qint64 end);

signals:
    void positionChanged(qint64 frame);
    void finished();
    // Emitted whenever audible looping starts/stops (play/stop/pause-toggle
    // of the loop flag), so the UI can reflect real loop activity.
    void loopStateChanged(bool active);

private:
    class FloatPcmDevice : public QIODevice {   // pull-mode source feeding QAudioSink
    public:
        FloatPcmDevice(AudioPlayer *owner);
    protected:
        qint64 readData(char *out, qint64 max) override;
        qint64 writeData(const char *, qint64) override { return -1; }
    private:
        AudioPlayer *owner_;
    };

    friend class FloatPcmDevice;
    void pumpAudio();
    qint64 generateBytes(char *out, qint64 maxBytes);

    const float *buf_ = nullptr;
    qint64 totalFrames_ = 0;
    int channels_ = 2;
    int sampleRate_ = 44100;

    QAudioSink *sink_ = nullptr;
    FloatPcmDevice *device_ = nullptr;
    SinkFactory sinkFactory_;
    VolumeApplier volumeApplier_;
    bool volumeApplierBusy_ = false;   // re-entrancy guard for device restarts

    // External-player fallback (aplay/ffplay pipe): push-mode pump.
    QIODevice *outDevice_ = nullptr;
    QAudioFormat outFmt_;
    QTimer *pump_ = nullptr;
    QByteArray pumpBuf_;           // bytes accepted by write() but not yet drained
    qint64 sinkQueuedFrames_ = 0;  // frames sitting in the output channel:
                                   // pumpBuf_ + everything handed to ProcessSink
                                   // (its stdin pipe + ALSA ring) that aplay has
                                   // not played yet. The pacing cap and the
                                   // audible-end bookkeeping both use this value.
    qint64 pumpBytesPerTick_ = 0;
    qint64 pumpFrameBytes_ = 0;
    int pumpPeriodMs_ = 20;
    // Sink-capacity pacing for the push pump. The cursor must follow the
    // AUDIBLE position, which trails the generated position by however much
    // PCM is currently sitting in the output channel (aplay's stdin pipe +
    // its ALSA ring buffer). A pure wall-clock pump ignores that reservoir:
    // at start the whole channel depth gets filled "on credit" within the
    // first few ticks (cursor sprints, then crawls, permanently ahead of the
    // sound by up to ~0.7 s), and empty timer ticks make it stutter. Instead
    // we cap the unplayed backlog at the channel capacity: generate one
    // period per tick ONLY while the backlog is below capacity, and idle
    // otherwise. Then the audible position equals posFrames_ minus a bounded
    // constant delay, the cursor never sprints, and seeks/pauses can drop
    // the backlog deterministically.
    QElapsedTimer playClock_;      // monotonic session clock (diagnostics/idle guard)
    qint64 clockBaseMs_ = 0;       // ms accumulated before the current run segment
    qint64 framesFed_ = 0;         // total frames handed to the sink this session
    qint64 lastPumpUsec_ = 0;      // previous pump tick (µs on playClock_)
    double pumpRateFramesPerUsec_ = 0.0;  // sampleRate / 1000
    qint64 pumpMaxBacklogFrames_ = 0;     // channel capacity in frames (pacing cap)
    // Consumption tracking for the push pump (see AudioPlayer::pumpAudio):
    // ProcessSink's cumulative handoff counter as observed on the previous
    // tick; its delta is what the child took from our write buffer into the
    // kernel pipe/ALSA ring since then. audibleFrames_ is the running maximum
    // of (generated - backlog): the drain estimate can only ever grow while a
    // session plays, so seeks/pauses must rebase it explicitly.
    qint64 lastHandedOff_ = 0;
    qint64 audibleFrames_ = 0;
    bool endPending_ = false;             // range generated, waiting for the
                                          // audible tail to drain before
                                          // declaring finished()

    // Frame where the most recent play session started; used by MainWindow
    // so that pressing Play after Stop resumes from that point.
    qint64 lastStartFrame() const { return startFrames_; }

    qint64 posFrames_ = 0;       // current playback position (kept after Stop)
    qint64 startFrames_ = 0;     // where the last Play session began (resume point)
    // Repeat boundary of the active session. Normally equals the session
    // range [sessStart_, sessEnd_]; a live Loop toggle may retarget it via
    // setLoopRange() so looping repeats exactly the selection-or-whole-file
    // range — never a stale end left over from an earlier seek or session.
    qint64 loopStart_ = 0, loopEnd_ = 0;
    // Session range as it was when audio actually started. A live seek may
    // retarget loopEnd_/startFrames_ (clicking outside the range while
    // playing); we keep the originals so pause/resume and stop() still use
    // the real session boundaries — notably so resume does NOT re-loop a
    // finished selection when Loop is off.
    qint64 sessStart_ = 0, sessEnd_ = 0;
    bool looping_ = false;
    bool playing_ = false;
    bool paused_ = false;        // paused mid-playback, resumable from posFrames_
    bool qtSinkMode_ = false;    // true: QAudioSink pull mode; false: timer pump
    bool firstPlayDiag_ = false; // one-time device-capability log on first play()
    std::atomic<double> volume_{0.8};
};
