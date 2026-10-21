#pragma once
#include <QAudioSink>
#include <QAudioFormat>
#include <QObject>
#include <QIODevice>
#include <QTimer>
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

    // Set the whole decoded buffer (interleaved float32).
    void setBuffer(const float *data, qint64 totalFrames, int channels, int sampleRate);

    void play(qint64 startFrame, qint64 endFrame, bool loop);  // end<=start => play to file end
    void pause();
    void resumeOrStopToggle();
    void stop();     // stops and returns position to last play-start point
    bool isPlaying() const { return playing_; }

    void setVolume(double v) { volume_ = v; }        // internal gain 0..1
    double volume() const { return volume_; }

    qint64 currentFrame() const { return posFrames_; }
    qint64 startFrame() const { return startFrames_; }

signals:
    void positionChanged(qint64 frame);
    void finished();

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

    // External-player fallback (aplay/ffplay pipe): push-mode pump.
    QIODevice *outDevice_ = nullptr;
    QAudioFormat outFmt_;
    QTimer *pump_ = nullptr;
    QByteArray pumpBuf_;
    qint64 pumpBytesPerTick_ = 0;
    qint64 pumpFrameBytes_ = 0;
    int pumpPeriodMs_ = 20;

    qint64 posFrames_ = 0;       // next frame to output
    qint64 startFrames_ = 0;     // where Play began (for Stop rewind)
    qint64 loopStart_ = 0, loopEnd_ = 0;
    bool looping_ = false;
    bool playing_ = false;
    double volume_ = 0.8;
};
