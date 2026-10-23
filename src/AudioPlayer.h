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
    // Push-mode generation pacing. The generator must not outrun real time:
    // the audible cursor trails it by exactly the measured in-flight backlog
    // (pumpBuf_ + QProcess queue + kernel pipe, see pumpAudio). If we fed
    // faster than the device plays, that backlog would grow to the channel
    // capacity (~64 KiB ≈ 0.7 s of S16 stereo @48k) and the cursor would sit
    // permanently ~0.7 s behind the generator — perceived as "cursor runs
    // ahead of the sound" because at startup the cursor is additionally held
    // back while aplay primes its ALSA ring. Two hard rules keep the true
    // in-flight latency at ~2 periods (~40 ms):
    //  * each tick generates AT MOST one period (maxPerTick), regardless of
    //    how much wall time elapsed since the previous tick — late ticks
    //    (event-loop stalls, process spawn inside writeData()) drop their
    //    shortfall instead of "catching up" with a burst that fills the
    //    whole channel;
    //  * the consumption gate caps the unplayed backlog below the channel
    //    capacity so generation idles whenever the child lags behind.
    QElapsedTimer playClock_;      // monotonic session clock (generation budget)
    qint64 clockBaseMs_ = 0;       // ms accumulated before the current run segment
    qint64 framesFed_ = 0;         // total frames generated this session
    qint64 lastPumpUsec_ = 0;      // previous pump tick (µs on playClock_)
    double pumpRateFramesPerUsec_ = 0.0;  // sampleRate / 1000
    qint64 pumpMaxBacklogFrames_ = 0;     // hard gate: max unplayed backlog
    qint64 pumpChannelReserveFrames_ = 0;  // aplay ALSA-ring estimate (frames)
    // Cursor model (push mode) — EXACT consumption accounting. posFrames_ is
    // what the UI paints, so it must equal the AUDIBLE position. Every byte
    // of session audio lives in exactly one place:
    //   our pumpBuf_  ->  QProcess queue  ->  kernel pipe  ->  aplay's ALSA
    //   ring          ->  the speaker.
    // ProcessSink measures the first three stages exactly each tick:
    //   handedOff  = bytes accepted by write() into the current child
    //   intoChild  = handedOff - QProcess queue   (left our process)
    //   pipeUnread = FIONREAD on the pipe fd      (still unread by aplay)
    // so "bytes the child demonstrably pulled out of the pipe" is exact:
    //   drained = intoChild - pipeUnread
    // and the audible frame position is drained minus a bounded estimate of
    // aplay's ALSA ring (reserve). All earlier models got one of these
    // corrections wrong and produced exactly the reported bugs:
    //  * wall-clock cursor pacing ignored the sink state entirely — during
    //    aplay's device-open window the cursor already ran at full speed
    //    ("sound starts later than the cursor");
    //  * handoff-delta pacing credited the cursor with bytes merely accepted
    //    by write(); the first chunk fills the whole 64 KiB pipe instantly
    //    and the cursor sprinted ahead for good;
    //  * the reserve-only variant still over-credited while the generator
    //    lagged behind real time, because it measured against `generated`,
    //    not against bytes demonstrably leaving the pipe.
    // Now the cursor parks at the play start until bytes physically leave
    // the pipe, then advances in lockstep with the device — evenly, because
    // the drain rate IS the sample rate. consumedFrames_ counts audible
    // frames since the session start; fedMinusConsumed_ is the measured
    // backlog (generated - audible) used by the generation gate;
    // endPending_ waits for the audible tail to reach the range end before
    // finishing, so the cursor lands exactly on the end when the tail goes
    // silent instead of teleporting back while stale audio still sounds.
    qint64 consumedFrames_ = 0;
    qint64 fedMinusConsumed_ = 0;
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
