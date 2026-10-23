#include "AudioPlayer.h"

#include "AudioDebug.h"
#include "ProcessSink.h"

#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QDebug>
#include <QTimer>
#include <cstring>
#include <algorithm>

namespace {
// Qt Multimedia on Ubuntu/Kubuntu (24.04 and later, incl. 25.10) is built
// against the LGPL/GPL-redistributed FFmpeg. Merely instantiating a media
// object (QAudioSink included) makes the ffmpeg backend print
//   "Using Qt multimedia with FFmpeg version ... GPL version 2 or later"
// through qFatal() — which ABORTS the process in debug/default builds. In
// release builds it only logs, but relying on that is fragile: an abort
// during play() kills both the Qt sink path and any fallback that has not
// started yet. So we never touch QAudioSink unless the environment says the
// crash-on-construction bug is absent (QT_MEDIA_BACKEND=ffmpeg/null/waveout)
// or the user explicitly forces the Qt path (AUDIOSCOUT_QTSINK=1).
bool qtMultimediaUnsafeEnv() {
    static const bool unsafe = [] {
        const QByteArray forced = qgetenv("AUDIOSCOUT_QTSINK").trimmed();
        if (!forced.isEmpty() && forced != "0") return false;   // user override
        const QByteArray backend = qgetenv("QT_MEDIA_BACKEND").trimmed().toLower();
        if (!backend.isEmpty())
            return backend == "ffmpeg";   // explicit ffmpeg backend still crashes
        return true;                      // unset/unknown: assume the broken build
    }();
    return unsafe;
}
}  // namespace

AudioPlayer::FloatPcmDevice::FloatPcmDevice(AudioPlayer *owner)
    : QIODevice(owner), owner_(owner) {}

qint64 AudioPlayer::FloatPcmDevice::readData(char *out, qint64 max) {
    return owner_->generateBytes(out, max);
}

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {}

AudioPlayer::~AudioPlayer() {
    delete sink_;
    // device_ is child of this
}

void AudioPlayer::setVolume(double v) {
    volume_.store(v);
    // Devices whose gain is fixed at startup (the "aplay" pipe fallback)
    // must be reconfigured live; the in-process float/Int16 paths read
    // volume_ directly on every generated chunk, so they need nothing more.
    if (volumeApplier_ && !volumeApplierBusy_) volumeApplier_(v);
}

// Recreate the push-mode output device (spawning the aplay/ffplay child
// process anew with the current volume) and resume feeding from the same
// playback position. Only meaningful while a session is active in pump mode.
void AudioPlayer::restartOutputKeepingPosition() {
    if (!outDevice_ || qtSinkMode_) return;
    // Legacy entry point kept for compatibility. Nothing calls it during
    // live sessions any more: in-process volume scaling (generateBytes)
    // made respawning the sink obsolete, and doing it per volume-slider
    // move terminated aplay mid-write (SIGTERM -> "write error: Interrupted
    // system call"). If it ever fires again, log it instead of silently
    // killing the audio child.
    qInfo() << "AudioPlayer: restarting output sink (legacy path) — this kills"
            << "the running child process; report as a bug if seen during play";
    const bool wasPlaying = playing_;
    const bool wasPaused = paused_;
    if (!wasPlaying && !wasPaused) return;

    // Re-entrancy guard: setVolume below (from play()) must not recurse
    // back into this function while we are mid-restart.
    volumeApplierBusy_ = true;
    if (pump_) pump_->stop();
    outDevice_->close();
    delete outDevice_;
    outDevice_ = nullptr;
    pumpBuf_.clear();

    auto failSession = [this] {
        playing_ = false; paused_ = false;
        emit loopStateChanged(false);
        emit finished();
    };
    if (!sinkFactory_) { volumeApplierBusy_ = false; failSession(); return; }
    // Always respawn the pipe as S16: the sink processes (aplay -f S16_LE,
    // ffplay s16le) consume signed 16-bit PCM and generateBytes() must emit
    // matching bytes — a stale Float outFmt_ here produced float data in an
    // S16 stream (distorted/silent audio after any volume-slider move).
    QAudioFormat fmt = outFmt_;
    fmt.setSampleFormat(QAudioFormat::Int16);
    outDevice_ = sinkFactory_(fmt);
    outFmt_ = fmt;
    if (!outDevice_) {
        qWarning() << "AudioPlayer: sink factory returned null on volume restart";
        volumeApplierBusy_ = false;
        failSession();
        return;
    }

    if (wasPlaying) {
        playing_ = true; paused_ = false;
        if (!pump_) {
            pump_ = new QTimer(this);
            connect(pump_, &QTimer::timeout, this, [this] { pumpAudio(); });
        }
        pump_->start(pumpPeriodMs_);
    } else {
        playing_ = false; paused_ = true;   // stay paused; resume() will restart
    }
    emit loopStateChanged(isLoopActive());
    volumeApplierBusy_ = false;
}

void AudioPlayer::setLooping(bool on) {
    if (looping_ == on) return;
    looping_ = on;
    emit loopStateChanged(isLoopActive());
}

// Retarget the repeat boundary of a live session without restarting audio.
// Only the loop range moves: the play-start point (startFrames_) and the
// session range stay untouched, so Stop still rewinds to where playback
// actually began and a non-looped resume keeps using the original boundaries.
bool AudioPlayer::setLoopRange(qint64 start, qint64 end) {
    if (!playing_ && !paused_) return false;
    start = std::clamp(start, qint64(0), totalFrames_ - 1);
    if (end <= start || end > totalFrames_) end = totalFrames_;
    loopStart_ = start;
    loopEnd_ = end;
    // If the audible position already passed the new repeat end (e.g. the
    // selection shrank while playing), snap back so the repeat starts now.
    if (posFrames_ >= loopEnd_ || posFrames_ < loopStart_) {
        posFrames_ = loopStart_;
        emit positionChanged(posFrames_);
    }
    return true;
}

void AudioPlayer::setBuffer(const float *data, qint64 totalFrames, int channels, int sampleRate) {
    stop();
    buf_ = data;
    totalFrames_ = totalFrames;
    channels_ = channels;
    sampleRate_ = sampleRate;
    posFrames_ = 0;
    startFrames_ = 0;
    delete sink_; sink_ = nullptr;
    if (outDevice_) { outDevice_->close(); delete outDevice_; outDevice_ = nullptr; }
}

void AudioPlayer::play(qint64 startFrame, qint64 endFrame, bool loop) {
    if (!buf_ || totalFrames_ == 0) return;
    startFrame = std::clamp(startFrame, qint64(0), totalFrames_ - 1);
    if (endFrame <= startFrame || endFrame > totalFrames_) endFrame = totalFrames_;

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate_);
    fmt.setChannelCount(channels_);
    fmt.setSampleFormat(QAudioFormat::Float);

    delete sink_;
    sink_ = nullptr;
    if (outDevice_) { outDevice_->close(); delete outDevice_; outDevice_ = nullptr; }

    // Preferred path: real QAudioSink on the default output device — but only
    // when constructing one is safe (see qtMultimediaUnsafeEnv above). On
    // Ubuntu/Kubuntu's ffmpeg-backed Qt Multimedia we skip it entirely and go
    // straight to the ALSA pipe sinks below, so playback never dies on the
    // backend's qFatal banner.
    const bool qtSafe = !qtMultimediaUnsafeEnv();
    const QAudioDevice dev = qtSafe ? QMediaDevices::defaultAudioOutput()
                                    : QAudioDevice{};
    if (qtSafe && !firstPlayDiag_) {
        firstPlayDiag_ = true;
        const QAudioFormat pref = dev.preferredFormat();
        DBG(QStringLiteral("Qt sink probe: default out device='%1' float=%2 int16=%3 preferred=%4Hz/%5ch/sf=%6")
            .arg(dev.description().isEmpty() ? QStringLiteral("<none>") : dev.description())
            .arg(dev.isFormatSupported(fmt) ? "yes" : "no")
            .arg([&] { QAudioFormat f16 = fmt; f16.setSampleFormat(QAudioFormat::Int16);
                       return dev.isFormatSupported(f16) ? "yes" : "no"; }())
            .arg(pref.sampleRate())
            .arg(pref.channelCount())
            .arg(int(pref.sampleFormat())));
    }
    bool useQtSink = false;
    if (!dev.isNull() && dev.isFormatSupported(fmt)) {
        sink_ = new QAudioSink(dev, fmt, this);
        useQtSink = true;
    } else if (!dev.isNull()) {
        // Device exists but doesn't accept float at this rate — try Int16.
        QAudioFormat f16 = fmt;
        f16.setSampleFormat(QAudioFormat::Int16);
        if (dev.preferredFormat().sampleFormat() == QAudioFormat::Int16 ||
            dev.isFormatSupported(f16)) {
            qWarning() << "AudioPlayer: float format unsupported, converting to Int16";
            fmt = f16;
            sink_ = new QAudioSink(dev, fmt, this);
            useQtSink = true;
        }
    }
    // Fallback path installed by MainWindow: the factory returns an already
    // opened write-only QIODevice (a pipe to "aplay"/"ffplay"). We push PCM
    // chunks into it from a timer-driven pump instead of using QAudioSink.
    // This is also the PRIMARY path on systems where Qt Multimedia aborts on
    // sink construction, so pick the format the pipe actually wants here.
    if (!useQtSink && sinkFactory_) {
        if (!qtSafe)
            DBG("Qt sink skipped: known-unsafe ffmpeg-backed Qt Multimedia build, using ALSA pipe sink");
        // The aplay/ffplay pipes always consume signed 16-bit PCM: request
        // that format up front. Leaving Float here makes generateBytes()
        // emit float bytes into an S16 stream — audible garbage/silence.
        QAudioFormat pipeFmt = fmt;
        pipeFmt.setSampleFormat(QAudioFormat::Int16);
        outDevice_ = sinkFactory_(pipeFmt);
        outFmt_ = pipeFmt;
    }
    if (!useQtSink && !outDevice_) {
        qWarning() << "AudioPlayer: no usable audio output (device null and no fallback)";
        return;
    }

    // Recreate the pull device for every play session (a fresh QIODevice
    // must be opened before being handed to QAudioSink::start()). Pull mode
    // needs it; push mode writes through outDevice_ directly.
    if (useQtSink) {
        delete device_;
        device_ = new FloatPcmDevice(this);
        if (!device_->open(QIODevice::ReadOnly)) {
            qWarning() << "AudioPlayer: cannot open PCM pull device";
            delete sink_; sink_ = nullptr;
            useQtSink = false;   // caller falls back via the watchdog anyway
            return;
        }
    }

    startFrames_ = startFrame;
    posFrames_ = startFrame;
    loopStart_ = startFrame;
    loopEnd_ = endFrame;
    sessStart_ = startFrame;
    sessEnd_ = endFrame;
    looping_ = loop;
    playing_ = true;
    paused_ = false;
    qtSinkMode_ = useQtSink;
    emit loopStateChanged(isLoopActive());

    // Let the output owner (e.g. an aplay pipe) apply the current volume to
    // the already-running process before audio starts flowing through it.
    if (volumeApplier_) volumeApplier_(volume_.load());

    if (useQtSink) {
        connect(sink_, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
            if ((st == QAudio::StoppedState || st == QAudio::IdleState) && playing_) {
                if (sink_ && sink_->error() != QAudio::NoError) {
                    qWarning() << "AudioPlayer: sink error" << sink_->error();
                }
                playing_ = false;
                emit loopStateChanged(false);
                emit finished();
            }
        });
        sink_->start(device_);
    } else {
        // Sink-capacity pacing (see pumpMaxBacklogFrames_ in the header):
        // generate at most one period per tick and never let the unplayed
        // backlog exceed the output channel. Start the monotonic session
        // clock for diagnostics/idle guarding; framesFed_ counts total audio
        // handed to the sink this session.
        if (!pump_) {
            pump_ = new QTimer(this);
            connect(pump_, &QTimer::timeout, this, [this] { pumpAudio(); });
        }
        pumpBuf_.clear();
        sinkQueuedFrames_ = 0;
        consumedFrames_ = 0;     // fresh session: nothing generated/audible yet
        fedMinusConsumed_ = 0;
        endPending_ = false;
        const qint64 periodBytes = qint64(sampleRate_ / 50) * channels_ * qint64(sizeof(float));
        pumpPeriodMs_ = 20;
        pumpFrameBytes_ = qint64(sizeof(float)) * channels_;
        if (outFmt_.sampleFormat() == QAudioFormat::Int16)
            pumpFrameBytes_ = qint64(sizeof(int16_t)) * channels_;
        else if (outFmt_.sampleFormat() == QAudioFormat::UInt8)
            pumpFrameBytes_ = channels_;
        pumpBytesPerTick_ = std::max(qint64(periodBytes), pumpFrameBytes_);
        pumpRateFramesPerUsec_ = sampleRate_ / 1000.0;
        // Generation pacing constants (see the header comment): budget one
        // period per tick from the wall clock, hard-gate the backlog at two
        // periods + the ALSA-ring reserve. The audible cursor is derived in
        // pumpAudio() from EXACT pipe accounting (FIONREAD), so during
        // aplay's startup window (device open + ring priming) it stays parked
        // at the play start while the channel fills — sound and playhead
        // begin together, and the true audio latency never exceeds ~2-3
        // periods (~40-60 ms).
        const qint64 maxPerTickFrames =
            std::max(qint64(1), pumpBytesPerTick_ / pumpFrameBytes_);
        pumpChannelReserveFrames_ = 3 * maxPerTickFrames;
        pumpMaxBacklogFrames_ =
            std::max(qint64(1), 2 * pumpBytesPerTick_ / pumpFrameBytes_) +
            pumpChannelReserveFrames_;
        framesFed_ = posFrames_ - startFrames_;
        if (framesFed_ < 0) framesFed_ = 0;
        clockBaseMs_ = 0;
        playClock_.restart();          // fresh, monotonic session clock
        lastPumpUsec_ = 0;
        DBG(QStringLiteral("AudioPlayer: push pump started (period=%1 bytes, %2 Hz, "
                           "backlogCap=%3 frames, reserve=%4 frames, framesFed=%5)")
                .arg(pumpBytesPerTick_).arg(sampleRate_)
                .arg(pumpMaxBacklogFrames_).arg(pumpChannelReserveFrames_)
                .arg(framesFed_));
        pump_->start(pumpPeriodMs_);
    }
}

// Timer-driven push-mode output for the external-player fallback sinks.
void AudioPlayer::pumpAudio() {
    if (!playing_ || !buf_) { if (pump_) pump_->stop(); return; }
    // EXACT consumption accounting drives the cursor. Every byte of session
    // audio is in one of five places: our pumpBuf_, the QProcess write queue,
    // the kernel stdin pipe, aplay's ALSA ring, or already heard. ProcessSink
    // measures the first three exactly each tick:
    //     handedOff   = bytes accepted by write() into the current child
    //     intoChild   = handedOff - QProcess queue        (left our process)
    //     pipeUnread  = FIONREAD on the pipe fd           (still unread)
    // so "bytes the child demonstrably pulled out of the pipe" is exact:
    //     drained = intoChild - pipeUnread
    // The audible position is that number minus what the ALSA ring can still
    // be holding unheard — bounded by our reserve estimate AND, crucially,
    // clamped so it can never pass the generator by more than the measured
    // in-flight backlog (pumpBuf_ + queue + pipe). Previous models skipped
    // one of these two corrections and produced exactly the reported bugs:
    // counting write()-accepted bytes as played made the cursor sprint ahead
    // while aplay was still opening the device ("cursor runs, sound starts
    // late, permanent desync"); ignoring startup entirely froze the pump at
    // the cap ("cursor sticks, no sound"). Now the cursor parks at the play
    // start until bytes physically leave the pipe, then advances in lockstep
    // with the device — evenly, because the drain rate IS the sample rate.
    const qint64 generatedFrames = framesFed_;   // frames produced this session
    bool endDrained = false;
    if (auto *ps = qobject_cast<ProcessSink *>(outDevice_)) {
        const qint64 fb = std::max(qint64(1), pumpFrameBytes_);
        const qint64 intoChild = ps->bytesIntoChild();   // left our process
        const qint64 pipeUnread = ps->bytesInPipe();     // unread pipe bytes
        const qint64 drainedBytes =
            std::max(qint64(0), intoChild - pipeUnread); // entered aplay
        // In-flight backlog between the generator position and aplay: bytes
        // we produced but the child has not taken yet (our buffer + queue +
        // pipe). Measured, not estimated.
        const qint64 inflightFrames =
            (pumpBuf_.size() + std::max(qint64(0), generatedFrames * fb - intoChild)) / fb;
        // Audible frames since session start: everything the child took out
        // of the pipe, minus the conservative ring reserve, never negative
        // and never beyond what the generator produced.
        qint64 aud = drainedBytes / fb -
                     std::min(pumpChannelReserveFrames_, inflightFrames);
        if (aud < 0) aud = 0;
        if (aud > generatedFrames) aud = generatedFrames;
        fedMinusConsumed_ = generatedFrames - aud;   // true backlog vs generator
        sinkQueuedFrames_ = fedMinusConsumed_;       // gate uses the same value
        if (aud > consumedFrames_) {                 // monotonic within a run
            consumedFrames_ = aud;
            const qint64 newPos = startFrames_ + consumedFrames_;
            if (newPos != posFrames_) {
                posFrames_ = newPos;
                emit positionChanged(posFrames_);
            }
        }
    } else {
        sinkQueuedFrames_ = 0;     // raw QIODevice sink: no way to observe drain
        if (posFrames_ != startFrames_ + framesFed_) {
            posFrames_ = startFrames_ + framesFed_;
            emit positionChanged(posFrames_);
        }
        consumedFrames_ = framesFed_;
    }
    // Audible end-of-range: the whole session range was generated but
    // its tail still sits in the output channel. Do NOT declare the
    // session finished while that tail plays — otherwise MainWindow
    // rewinds the cursor (and re-issues play(), whose stop() first cuts
    // the stale tail short) while the file end is still sounding. Once
    // the backlog drains, finish normally. The guard timer below keeps
    // a wedged child from hanging the session forever.
    if (endPending_) {
        // Finish when the audible position reached the range end (the tail
        // went through the speaker) — or, failing that, once every byte has
        // demonstrably left the pipe and our buffers are empty.
        const qint64 gen = startFrames_ + framesFed_;
        endDrained = consumedFrames_ >= framesFed_ ||
                     (fedMinusConsumed_ == 0 && pumpBuf_.isEmpty());
        if (endDrained && pumpBuf_.isEmpty()) {
            posFrames_ = gen;   // cursor lands exactly on the range end
            emit positionChanged(posFrames_);
            DBG(QStringLiteral("AudioPlayer: audible tail drained at frame %1 "
                               "(%2 ms after range end) — finishing session")
                    .arg(posFrames_)
                    .arg(playClock_.isValid() ? playClock_.elapsed() : 0));
            endPending_ = false;
            playing_ = false;
            paused_ = false;
            if (pump_) pump_->stop();
            emit loopStateChanged(false);
            emit finished();
            return;
        }
        if (playClock_.isValid() &&
            clockBaseMs_ + playClock_.elapsed() - lastPumpUsec_ / 1000 > 3000) {
            DBG("AudioPlayer: end-of-range drain wait timed out (3 s) — forcing finish");
            endPending_ = false;
            playing_ = false;
            paused_ = false;
            if (pump_) pump_->stop();
            emit loopStateChanged(false);
            emit finished();
            return;
        }
    }
    if (!outDevice_) { if (pump_) pump_->stop(); return; }
    // Produce PCM in native sink format via the same generator as pull mode.
    // While the end-tail drains we keep feeding pumpBuf_ into the child
    // (generation itself already stopped at the range end), so the drain
    // condition above can actually be reached.
    QByteArray tmp;
    if (!endPending_) {
        tmp.resize(int(pumpBytesPerTick_));
        const qint64 produced = generateBytes(tmp.data(), pumpBytesPerTick_);
        if (produced < 0) {   // end-of-range with an audible tail: see below
            if (pump_) pump_->stop();
            return;
        }
        if (produced == 0) {
            if (!playing_) {   // generateBytes emitted finished() — stop pumping
                if (pump_) pump_->stop();
            }
            // fall through: still retry any bytes stuck in pumpBuf_
        } else {
            tmp.resize(int(produced));
            pumpBuf_.append(tmp);
        }
    }
    if (pumpBuf_.isEmpty()) return;
    const qint64 written = outDevice_->write(pumpBuf_);
    if (written < 0) {     // external player died — stop gracefully
        qWarning() << "AudioPlayer: fallback sink write failed";
        if (pump_) pump_->stop();
        playing_ = false;
        emit loopStateChanged(false);
        emit finished();
        return;
    }
    // Partial/zero writes mean the child's pipe is momentarily full (it is
    // catching up with the ALSA device). Keep the remaining bytes buffered —
    // bounded, because the backlog cap only ever lets ~one period pile up —
    // and retry them on the next pump tick instead of losing audio.
    if (written < pumpBuf_.size()) {
        if (pumpBuf_.size() - written > pumpBytesPerTick_ * 16)
            pumpBuf_.remove(0, int(pumpBuf_.size() - pumpBytesPerTick_ * 16));
        pumpBuf_.remove(0, int(written));
    } else {
        pumpBuf_.clear();
    }
}

qint64 AudioPlayer::generateBytes(char *out, qint64 maxBytes) {
    if (!playing_ || !buf_) {
        if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
        return 0;
    }
    const int bytesPerFrame = channels_ * int(sizeof(float));

    // Push-mode fallback: produce PCM in the external sink's native format.
    // NOTE: must consult outDevice_ (the fallback flag) — sink_ may be a
    // non-null leftover from an earlier Qt-sink session or from the Int16
    // probe path, which would otherwise hijack this branch and write float
    // bytes into an S16 stream (distorted playback).
    if (outDevice_) {
        const bool i16 = outFmt_.sampleFormat() == QAudioFormat::Int16;
        const bool u8 = outFmt_.sampleFormat() == QAudioFormat::UInt8;
        const qint64 fbBytesPerFrame = channels_ * (i16 ? qint64(2) : u8 ? qint64(1) : qint64(4));
        // Push mode has TWO positions: the generator position `genFrames_`
        // (frames produced/handed toward the sink since the session start —
        // what decides how much PCM to render this tick and when the range
        // is exhausted) and the AUDIBLE cursor `posFrames_`, which pumpAudio()
        // derives from EXACT pipe accounting (bytes demonstrably leaving the
        // kernel pipe minus the ALSA-ring reserve) and the UI paints. The old
        // code conflated them: posFrames_ was advanced by the wall clock while
        // audio piled up in the pipe, so the cursor sprinted ahead of the
        // sound at startup ("sound starts later than the cursor") and tele-
        // ported on late timer ticks ("cursor stutters"). Now generation runs
        // at real time with a hard one-period-per-tick ceiling, and the
        // cursor only ever moves by proven consumption.
        const qint64 genFrames_ = startFrames_ + framesFed_;
        qint64 wantFrames = 0;
        const qint64 maxPerTick =
            std::max(qint64(1), pumpBytesPerTick_ / fbBytesPerFrame);
        if (playClock_.isValid()) {
            const qint64 nowUsec = (clockBaseMs_ + playClock_.elapsed()) * 1000;
            const qint64 dueUsec = std::max(qint64(0), nowUsec - lastPumpUsec_);
            lastPumpUsec_ = nowUsec;
            wantFrames = qint64(dueUsec * pumpRateFramesPerUsec_);
            // HARD one-period ceiling per tick: a late tick never "catches
            // up" with a burst. Without it, any event-loop stall (aplay spawn
            // inside writeData(), repaint work) lets the next tick generate
            // seconds of PCM at once, filling the entire channel — the cursor
            // then trails the generator by the full backlog cap for the rest
            // of the session ("cursor runs ahead of the sound"). Shortfalls
            // are dropped; the consumption gate below keeps the stream
            // continuous across ordinary jitter anyway.
            wantFrames = std::min(wantFrames, maxPerTick);
            // Consumption gate: never keep more unplayed audio in flight than
            // the channel capacity allows. sinkQueuedFrames_ is the MEASURED
            // backlog (generated - audible, see pumpAudio); once it reaches
            // the cap the pump idles for whole ticks until the child drains
            // more, so generation self-synchronises to the device's rate.
            const qint64 room = pumpMaxBacklogFrames_ - sinkQueuedFrames_;
            if (room <= 0) return 0;    // channel full: wait for the drain
            wantFrames = std::min(wantFrames, room);
        } else {   // clock not running (legacy/manual caller): byte budget
            wantFrames = maxBytes / fbBytesPerFrame;
            wantFrames = std::min(wantFrames, maxPerTick);
        }
        qint64 framesAvail = loopEnd_ - genFrames_;
        // Push mode: the *audible* end is what matters. Once the generator
        // reaches loopEnd_ but PCM from this range still sits in the output
        // channel (pumpBuf_/pipe backlog), arm endPending_: pumpAudio()
        // watches the backlog drain and only then declares finished(). This
        // keeps the cursor walking to the range end while the tail plays,
        // instead of teleporting back to the start with the file end still
        // sounding ("play to the end: cursor jumps back, next play starts
        // with leftover audio from the old end"). Return 0 so the caller
        // skips generation but keeps flushing/writing pumpBuf_.
        if (framesAvail <= 0 && !looping_ &&
            (sinkQueuedFrames_ > 0 || !pumpBuf_.isEmpty())) {
            endPending_ = true;
            return 0;   // tail still audible: idle ticks only
        }
        if (framesAvail <= 0) {
            if (looping_) {
                posFrames_ = loopStart_;
                framesFed_ = 0;
                consumedFrames_ = 0;     // fresh repeat: channel emptied itself
                fedMinusConsumed_ = 0;   // at the wrap point (backlog < cap)
                if (auto *ps = qobject_cast<ProcessSink *>(outDevice_)) {
                    ps->flushPipe(/*force=*/true);
                    sinkQueuedFrames_ = 0;
                }
                DBG(QStringLiteral("AudioPlayer: loop wrap to frame %1 "
                                   "(channel flushed, cursor rebased)")
                        .arg(posFrames_));
                emit positionChanged(posFrames_);
                framesAvail = loopEnd_ - loopStart_;
            } else { playing_ = false; emit loopStateChanged(false); emit finished(); return -1; }
        }
        wantFrames = std::min(wantFrames, framesAvail);
        if (wantFrames <= 0) {
            // Nothing due this tick (early tick of the timer, sub-millisecond
            // rounding, or the channel is momentarily full). Return 0 WITHOUT
            // touching posFrames_ — emitting positionChanged on empty chunks
            // made the cursor stutter ("moves unevenly").
            return 0;
        }
        const float *src = buf_ + genFrames_ * channels_;
        const double v = volume_.load();
        if (i16) {
            int16_t *dst = reinterpret_cast<int16_t *>(out);
            for (qint64 i = 0; i < wantFrames * channels_; ++i) {
                float s = float(std::clamp(src[i] * v, -1.0, 1.0));
                dst[i] = int16_t(s >= 0 ? s * 32767.0f : s * 32768.0f);
            }
        } else if (u8) {
            uint8_t *dst = reinterpret_cast<uint8_t *>(out);
            for (qint64 i = 0; i < wantFrames * channels_; ++i) {
                float s = float(std::clamp(src[i] * v, -1.0, 1.0));
                dst[i] = uint8_t((s + 1.0f) * 127.5f);
            }
        } else {
            float *dst = reinterpret_cast<float *>(out);
            for (qint64 i = 0; i < wantFrames * channels_; ++i)
                dst[i] = float(src[i] * v);
        }
        framesFed_ += wantFrames;   // generator position advances here;
        // posFrames_ (the audible cursor) is updated by pumpAudio() from the
        // child's consumption counters, one tick behind at most by design.
        return wantFrames * fbBytesPerFrame;
    }

    qint64 framesAvail = loopEnd_ - posFrames_;
    if (framesAvail <= 0) {
        if (looping_) {
            posFrames_ = loopStart_;
            framesAvail = loopEnd_ - posFrames_;
        } else {
            playing_ = false;
            emit loopStateChanged(false);
            emit finished();
            return 0;
        }
    }
    qint64 wantFrames = std::min(maxBytes / bytesPerFrame, framesAvail);
    if (wantFrames <= 0) return 0;

    const float *src = buf_ + posFrames_ * channels_;
    const double v = volume_.load();

    // If the sink was configured for Int16 output (device doesn't support
    // float), convert on the fly.
    if (sink_ && sink_->format().sampleFormat() == QAudioFormat::Int16) {
        int16_t *dst = reinterpret_cast<int16_t *>(out);
        for (qint64 i = 0; i < wantFrames * channels_; ++i) {
            float s = float(std::clamp(src[i] * v, -1.0, 1.0));
            dst[i] = int16_t(s >= 0 ? s * 32767.0f : s * 32768.0f);
        }
    } else {
        float *dst = reinterpret_cast<float *>(out);
        for (qint64 i = 0; i < wantFrames * channels_; ++i)
            dst[i] = float(src[i] * v);
    }

    posFrames_ += wantFrames;
    emit positionChanged(posFrames_);
    return wantFrames * channels_ *
           (sink_ && sink_->format().sampleFormat() == QAudioFormat::Int16
                ? qint64(sizeof(int16_t)) : qint64(sizeof(float)));
}

void AudioPlayer::pause() {
    if (!playing_) return;
    playing_ = false;
    paused_ = true;
    if (pump_) pump_->stop();
    // Freeze the wall clock: remember how much real playback time elapsed
    // so resume() continues from the same audible point instead of counting
    // the paused interval as played audio.
    if (!qtSinkMode_ && playClock_.isValid()) {
        clockBaseMs_ += playClock_.elapsed();
        playClock_.invalidate();
    }
    if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
    // Push mode: freeze the in-flight PCM too. Without this, up to ~64 KiB
    // of audio already sitting in the aplay child's stdin pipe keeps
    // playing after pause ("Stop/pause is silent only seconds later"), and
    // resume would then continue from posFrames_ while stale bytes from the
    // pre-pause position had just been heard. The child respawns lazily on
    // the next write (resume()), so it also releases the ALSA device while
    // paused — other apps can use it.
    if (!qtSinkMode_ && outDevice_) {
        pumpBuf_.clear();
        endPending_ = false;
        if (auto *ps = qobject_cast<ProcessSink *>(outDevice_)) {
            // `force`: interrupt the child even when QProcess reports 0 bytes
            // queued — the kernel pipe + aplay's ALSA ring still hold up to
            // ~0.5 s of PCM, and without the kill the sound keeps playing for
            // that long after the cursor froze ("Pause stops the cursor but
            // not the audio"). The child is killed here and respawns lazily
            // on resume(), so all handoff counters restart at 0 as well.
            ps->flushPipe(/*force=*/true);
        }
        sinkQueuedFrames_ = 0;
        fedMinusConsumed_ = 0;
        consumedFrames_ = framesFed_;   // everything generated before the
        if (consumedFrames_ < 0)        // forced flush counts as heard: it
            consumedFrames_ = 0;        // was dropped down the dead channel
    }
}

void AudioPlayer::resume() {
    if (playing_ || !paused_) return;
    // Continue from the position where we paused; keep the original
    // play-start point so Stop still rewinds there (spec 2.6). Use the
    // session's *original* range, not the possibly seek-retargeted one: a
    // selection that already finished must not re-loop when Loop is off.
    if (!looping_ && posFrames_ >= sessEnd_) posFrames_ = sessStart_;
    paused_ = false;
    playing_ = true;
    if (qtSinkMode_) {
        if (device_ && !device_->isOpen()) device_->open(QIODevice::ReadOnly);
        sink_->start(device_);
    } else {
        // Re-anchor the wall clock: the paused interval must not count as
        // played audio, and framesFed_ keeps tracking total audio handed to
        // the sink (the generation position relative to the session start).
        framesFed_ = posFrames_ - startFrames_;
        if (framesFed_ < 0) framesFed_ = 0;
        clockBaseMs_ = 0;              // resume() already folded the paused
                                       // segment away: new continuous base
        playClock_.restart();
        lastPumpUsec_ = 0;
        // The backlog was force-flushed at pause: nothing generated so far is
        // sitting unheard in the channel anymore, and the lazily respawned
        // child restarts its consumption counters from zero. Rebase the
        // cursor bookkeeping so the fresh stream starts moving the playhead
        // again from the paused position.
        sinkQueuedFrames_ = 0;
        endPending_ = false;
        consumedFrames_ = framesFed_;
        fedMinusConsumed_ = 0;
        if (!pump_) {
            pump_ = new QTimer(this);
            connect(pump_, &QTimer::timeout, this, [this] { pumpAudio(); });
        }
        pump_->start(pumpPeriodMs_ > 0 ? pumpPeriodMs_ : 20);
    }
}

void AudioPlayer::seek(qint64 frame) {
    if (!buf_ || totalFrames_ == 0) return;
    frame = std::clamp(frame, qint64(0), totalFrames_ - 1);
    // A seek never invents a new play-start point: seeking *backwards* keeps
    // it (so Stop still rewinds there), but seeking forward — e.g. clicking
    // past the end of a finished selection — must NOT extend the session
    // range. Playback then simply runs to the session's own end and stops,
    // which is what makes the cursor honour the selection boundary.
    if ((playing_ || paused_) && frame < startFrames_)
        startFrames_ = frame;
    posFrames_ = frame;
    if (!playing_ && !paused_) {
        // No active session: just remember the position for the next resume.
        emit positionChanged(posFrames_);
        return;
    }
    if (qtSinkMode_ && sink_) {
        // Pull mode: QAudioSink keeps its own ring buffer of already-fed PCM,
        // so we must restart the stream to make the new position take effect
        // immediately. Reuse the existing device when it is still open; after
        // pause()/stop() it is closed and must be recreated (Qt Multimedia
        // forbids reopening a QIODevice that was closed after start()).
        if (!device_ || !device_->isOpen()) {
            delete device_;
            device_ = new FloatPcmDevice(this);
            if (!device_->open(QIODevice::ReadOnly)) {
                qWarning() << "AudioPlayer: cannot reopen PCM pull device on seek";
                emit positionChanged(posFrames_);
                return;
            }
        }
        sink_->stop();
        if (playing_) sink_->start(device_);   // paused: stay stopped, resume() restarts
    } else if (outDevice_) {
        // Push mode: drop the buffered-but-not-yet-written PCM so the jump is
        // heard promptly; the timer pump simply feeds from the new position.
        const bool hadQueued = !pumpBuf_.isEmpty();
        pumpBuf_.clear();
        // Re-anchor the wall clock at the new position: everything fed to
        // the sink before this point is stale (flushed below), so the pacing
        // budget restarts here — otherwise generateBytes() keeps counting
        // against the old session start and the cursor drifts after seeks.
        if (playClock_.isValid())
            clockBaseMs_ += playClock_.elapsed();
        framesFed_ = posFrames_;
        lastPumpUsec_ = clockBaseMs_ * 1000;   // µs on the continuous clock
        DBG(QStringLiteral("AudioPlayer: push seek re-anchored clock to frame %1 (%2 ms)")
                .arg(posFrames_).arg(clockBaseMs_));
        // Also flush everything already handed to the external sink process:
        // its stdin pipe holds up to ~64 KiB of stale PCM from the OLD
        // position, which aplay would otherwise keep playing for hundreds of
        // milliseconds after the seek ("cursor moved, sound lags behind").
        // The ProcessSink respawns the child lazily on the next write.
        // `force` when the player was actively running: even with 0 bytes
        // queued in QProcess, the kernel pipe + aplay's ALSA ring still hold
        // stale audio; waiting for that drain (old EOF path) froze the UI
        // for ~500 ms per click before the cursor jumped. A forced flush
        // interrupts the child instantly instead. While paused there is no
        // live stream to interrupt — keep the cheap skip-if-empty behaviour.
        if (auto *ps = qobject_cast<ProcessSink *>(outDevice_)) {
            ps->flushPipe(/*force=*/playing_ || hadQueued);
            // Everything generated before the jump is gone down the channel:
            // rebase the backlog bookkeeping at the new position so the pump
            // starts feeding afresh instead of believing the channel is full.
            sinkQueuedFrames_ = 0;
            endPending_ = false;
            consumedFrames_ = posFrames_ - startFrames_;   // cursor rebases to
            if (consumedFrames_ < 0) consumedFrames_ = 0;  // the seek position
            fedMinusConsumed_ = 0;
        }
    }
    emit positionChanged(posFrames_);
}

void AudioPlayer::resumeOrStopToggle() {
    if (playing_) pause();
    else if (paused_) resume();
    else play(sessStart_, looping_ ? loopEnd_ : sessEnd_, looping_);
}

void AudioPlayer::stop() {
    const bool hadSession = playing_ || paused_;
    if (pump_) pump_->stop();
    playClock_.invalidate();   // session clock stops with the session
    if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
    if (outDevice_) {
        // Stop must be *instantly* silent: close() only sends EOF, letting
        // aplay keep draining everything buffered in the pipe — audible for
        // up to ~64 KiB (~0.37 s) after the button press. Flush first, then
        // close/delete as usual. `force`: interrupt even with 0 bytes queued
        // on our side — the kernel pipe + aplay's ALSA ring still hold stale
        // audio from the old position and must not keep playing (and must
        // not make us wait for their drain before the cursor rewinds).
        // Always force: stop is taken both mid-session AND right after a
        // natural end-of-file (MainWindow rewinds the cursor via play()),
        // when playing_ is already false but the sink child is still alive
        // with the file tail sitting unplayed in its stdin pipe — exactly
        // the "next Play starts with leftover audio from the old end" bug.
        if (auto *ps = qobject_cast<ProcessSink *>(outDevice_)) {
            ps->flushPipe(/*force=*/true);
        }
        pumpBuf_.clear();
        sinkQueuedFrames_ = 0;
        consumedFrames_ = 0;     // no session: nothing generated/audible
        fedMinusConsumed_ = 0;
        endPending_ = false;
        outDevice_->close(); delete outDevice_; outDevice_ = nullptr;
    }
    playing_ = false;
    paused_ = false;
    emit loopStateChanged(isLoopActive());   // session over: loop is no longer active
    // Leave posFrames_ where it is: MainWindow decides what the cursor does
    // on Stop (rewind to the session start) and on natural end-of-selection
    // (also rewind to the start). Emitting here only when a session actually
    // existed prevents a spurious cursor jump on redundant stop() calls —
    // e.g. loadCurrentFile() stops an idle player, which would otherwise
    // snap a freshly created selection's cursor back to frame 0.
    if (hadSession)
        emit positionChanged(posFrames_);
}
