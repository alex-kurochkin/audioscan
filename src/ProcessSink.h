#pragma once
#include <QAudioFormat>
#include <QIODevice>
#include <QProcess>
#include <QStringList>

// Fallback sinks: when Qt Multimedia has no usable audio output device
// (missing PulseAudio/PipeWire plugins), pipe raw PCM to an external player
// via a QProcess-backed QIODevice. QAudioSink accepts any QIODevice that
// supports write, so this keeps playback working even in bare environments.
class ProcessSink : public QIODevice {
public:
    explicit ProcessSink(const QString &program, const QStringList &args,
                         QObject *parent = nullptr);
    ~ProcessSink() override;

    // Restart the child process with an updated argument list while keeping
    // the same object identity (AudioPlayer deletes this device only when it
    // replaces it). Used for live volume changes on "aplay", whose -volume is
    // fixed at spawn time. The next writeData() respawns lazily with newArgs.
    void restartWithArgs(const QStringList &newArgs);

protected:
    qint64 readData(char *, qint64) override;
    qint64 writeData(const char *data, qint64 len) override;
    void close() override;

private:
    bool ensureStarted();
    void shutdown();
    QString program_;
    QStringList args_;
    QProcess *proc_ = nullptr;
};

// Build the "aplay" raw-PCM argument list for a format and linear volume.
QStringList aplayArgsFor(const QAudioFormat &fmt, double volume);
// Create an opened write-only sink piping PCM to "aplay" (ALSA) or "ffplay".
QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent);
QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent);
