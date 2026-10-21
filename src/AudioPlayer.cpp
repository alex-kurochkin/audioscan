#include "AudioPlayer.h"

#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QDebug>
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

void AudioPlayer::setBuffer(const float *data, qint64 totalFrames, int channels, int sampleRate) {
    stop();
    buf_ = data;
    totalFrames_ = totalFrames;
    channels_ = channels;
    sampleRate_ = sampleRate;
    posFrames_ = 0;
    startFrames_ = 0;
    delete sink_; sink_ = nullptr;
}

void AudioPlayer::play(qint64 startFrame, qint64 endFrame, bool loop) {
    if (!buf_ || totalFrames_ == 0) return;
    startFrame = std::clamp(startFrame, qint64(0), totalFrames_ - 1);
    if (endFrame <= startFrame || endFrame > totalFrames_) endFrame = totalFrames_;

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate_);
    fmt.setChannelCount(channels_);
    fmt.setSampleFormat(QAudioFormat::Float);

    // Verify the format is supported by the default audio device.
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!dev.isFormatSupported(fmt)) {
        qWarning() << "AudioPlayer: format" << sampleRate_ << "Hz /"
                   << channels_ << "ch float not supported by output device";
        return;
    }

    delete sink_;
    sink_ = new QAudioSink(dev, fmt, this);

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

    connect(sink_, &QAudioSink::stateChanged, this, [this](QAudio::State st) {
        if ((st == QAudio::StoppedState || st == QAudio::IdleState) && playing_) {
            if (sink_ && sink_->error() != QAudio::NoError) {
                qWarning() << "AudioPlayer: sink error" << sink_->error();
            }
            playing_ = false;
            emit finished();
        }
    });
    sink_->start(device_);
}

qint64 AudioPlayer::generateBytes(char *out, qint64 maxBytes) {
    if (!playing_ || !buf_) {
        if (sink_) sink_->stop();
        return 0;
    }
    const int bytesPerFrame = channels_ * int(sizeof(float));
    qint64 framesAvail = loopEnd_ - posFrames_;
    if (framesAvail <= 0) {
        if (looping_) {
            posFrames_ = loopStart_;
            framesAvail = loopEnd_ - posFrames_;
        } else {
            playing_ = false;
            emit finished();
            return 0;
        }
    }
    qint64 wantFrames = std::min(maxBytes / bytesPerFrame, framesAvail);
    if (wantFrames <= 0) return 0;

    const float *src = buf_ + posFrames_ * channels_;
    float *dst = reinterpret_cast<float *>(out);
    std::memcpy(dst, src, size_t(wantFrames * bytesPerFrame));
    // apply internal volume gain
    const double v = volume_;
    for (qint64 i = 0; i < wantFrames * channels_; ++i)
        dst[i] = float(src[i] * v);

    posFrames_ += wantFrames;
    emit positionChanged(posFrames_);
    return wantFrames * bytesPerFrame;
}

void AudioPlayer::pause() {
    if (sink_ && playing_) {
        sink_->suspend();
        playing_ = false;   // generateBytes returns 0 -> effectively paused
        sink_->stop();
    }
}

void AudioPlayer::resumeOrStopToggle() {
    if (playing_) pause();
    else play(startFrames_, loopEnd_, looping_);
}

void AudioPlayer::stop() {
    if (sink_) sink_->stop();
    playing_ = false;
    posFrames_ = startFrames_;   // rewind to the point where Play started
    emit positionChanged(posFrames_);
}
