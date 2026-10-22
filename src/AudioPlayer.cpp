#include "AudioPlayer.h"

#include "AudioDebug.h"

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
        // Realtime pacing: feed exactly one period of audio per timer tick.
        if (!pump_) {
            pump_ = new QTimer(this);
            connect(pump_, &QTimer::timeout, this, [this] { pumpAudio(); });
        }
        pumpBuf_.clear();
        const qint64 periodBytes = qint64(sampleRate_ / 50) * channels_ * qint64(sizeof(float));
        pumpPeriodMs_ = 20;
        pumpFrameBytes_ = qint64(sizeof(float)) * channels_;
        if (outFmt_.sampleFormat() == QAudioFormat::Int16)
            pumpFrameBytes_ = qint64(sizeof(int16_t)) * channels_;
        else if (outFmt_.sampleFormat() == QAudioFormat::UInt8)
            pumpFrameBytes_ = channels_;
        pumpBytesPerTick_ = std::max(qint64(periodBytes), pumpFrameBytes_);
        pump_->start(pumpPeriodMs_);
    }
}

// Timer-driven push-mode output for the external-player fallback sinks.
void AudioPlayer::pumpAudio() {
    if (!playing_ || !outDevice_ || !buf_) { if (pump_) pump_->stop(); return; }
    // Produce PCM in native sink format via the same generator as pull mode.
    QByteArray tmp;
    tmp.resize(int(pumpBytesPerTick_));
    const qint64 produced = generateBytes(tmp.data(), pumpBytesPerTick_);
    if (produced <= 0) {   // playback reached the end (generateBytes emitted finished)
        if (pump_) pump_->stop();
        return;
    }
    tmp.resize(int(produced));
    pumpBuf_.append(tmp);
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
    // bounded, because we only ever feed ~one tick per call — and retry them
    // on the next pump tick instead of losing audio.
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
        qint64 framesAvail = loopEnd_ - posFrames_;
        if (framesAvail <= 0) {
            if (looping_) { posFrames_ = loopStart_; framesAvail = loopEnd_ - posFrames_; }
            else { playing_ = false; emit loopStateChanged(false); emit finished(); return 0; }
        }
        qint64 wantFrames = std::min(maxBytes / fbBytesPerFrame, framesAvail);
        if (wantFrames <= 0) return 0;
        const float *src = buf_ + posFrames_ * channels_;
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
        posFrames_ += wantFrames;
        emit positionChanged(posFrames_);
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
    if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
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
        pumpBuf_.clear();
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
    if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
    if (outDevice_) { outDevice_->close(); delete outDevice_; outDevice_ = nullptr; }
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
