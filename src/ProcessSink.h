#pragma once
#include <QAudioFormat>
#include <QIODevice>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

// Fallback sinks: when Qt Multimedia has no usable audio output device
// (missing PulseAudio/PipeWire plugins), pipe raw PCM to an external player
// via a QProcess-backed QIODevice. QAudioSink accepts any QIODevice that
// supports write, so this keeps playback working even in bare environments.
class ProcessSink : public QIODevice {
    Q_OBJECT
public:
    explicit ProcessSink(const QString &program, const QStringList &args,
                         QObject *parent = nullptr);
    ~ProcessSink() override;

    // Restart the child process with an updated argument list while keeping
    // the same object identity (AudioPlayer deletes this device only when it
    // replaces it). The next writeData() respawns lazily with newArgs.
    void restartWithArgs(const QStringList &newArgs);

    // Discard all not-yet-flushed bytes and force the child to stop feeding
    // stale PCM (used on seek/stop so a position jump is heard immediately).
    // If `force` is true the child is interrupted even when nothing is queued
    // on our side: the kernel pipe plus aplay's own ALSA ring can still hold
    // up to ~0.5 s of audio from the OLD position, which must not keep
    // playing after a click-to-seek ("cursor jumps late" symptom).
    void flushPipe(bool force = false);
    // Bytes accepted by write() but still queued for the child's stdin.
    qint64 discardBuffered();

protected:
    qint64 readData(char *, qint64) override;
    qint64 writeData(const char *data, qint64 len) override;
    void close() override;

private:
    bool ensureStarted();
    void shutdown();
    // Synchronously finish the child (EOF -> wait -> terminate -> kill).
    // Needed because shutdown() only defers escalation via a QTimer, which
    // never fires if the sink object is destroyed right after.
    void reapChild(qint64 waitMs);
    QString program_;
    QStringList args_;
    QProcess *proc_ = nullptr;
    bool started_ = false;     // child was successfully spawned at least once
    bool finishing_ = false;   // EOF sent; deferred SIGTERM escalation armed
    bool flushedRecently_ = false; // last spawn followed flushPipe(): warm start
};

// Build the "aplay" raw-PCM argument list for a format. The volume parameter
// is vestigial (ignored): Ubuntu's alsa-utils is built --disable-softvol and
// its aplay has no gain flag; gain is applied in-process before the pipe.
QStringList aplayArgsFor(const QAudioFormat &fmt, double volume = 1.0);
// Create an opened write-only sink piping PCM to "aplay" (ALSA) or "ffplay".
QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent);
QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent);
