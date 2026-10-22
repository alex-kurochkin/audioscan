#include "AudioPlayer.h"

#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QDebug>
#include <QTimer>
#include <cstring>
#include <algorithm>

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
    if (volumeApplier_) volumeApplier_(v);
}

void AudioPlayer::setLooping(bool on) {
    if (looping_ == on) return;
    looping_ = on;
    emit loopStateChanged(isLoopActive());
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

    // Preferred path: real QAudioSink on the default output device.
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
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
    if (!useQtSink && sinkFactory_) {
        outDevice_ = sinkFactory_(fmt);
        outFmt_ = fmt;
    }
    if (!useQtSink && !outDevice_) {
        qWarning() << "AudioPlayer: no usable audio output (device null and no fallback)";
        return;
    }

    // Recreate the pull device for every play session (a fresh QIODevice
    // must be opened before being handed to QAudioSink::start()).
    delete device_;
    device_ = new FloatPcmDevice(this);
    if (!device_->open(QIODevice::ReadOnly)) {
        qWarning() << "AudioPlayer: cannot open PCM pull device";
        return;
    }

    startFrames_ = startFrame;
    posFrames_ = startFrame;
    loopStart_ = startFrame;
    loopEnd_ = endFrame;
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
    if (written < pumpBuf_.size())
        pumpBuf_.remove(0, int(written));
    else
        pumpBuf_.clear();
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
    // play-start point so Stop still rewinds there (spec 2.6).
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
    else play(startFrames_, loopEnd_, looping_);
}

void AudioPlayer::stop() {
    if (pump_) pump_->stop();
    if (sink_ && sink_->state() != QAudio::StoppedState) sink_->stop();
    if (outDevice_) { outDevice_->close(); delete outDevice_; outDevice_ = nullptr; }
    playing_ = false;
    paused_ = false;
    emit loopStateChanged(isLoopActive());   // session over: loop is no longer active
    posFrames_ = startFrames_;   // rewind to the point where Play started
    emit positionChanged(posFrames_);
}
