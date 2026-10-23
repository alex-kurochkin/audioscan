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
    // Bytes still queued at OUR side (QProcess write buffer): accepted by
    // write() but not yet handed to the kernel pipe. Non-destructive — safe
    // to call on every pump tick. Everything beyond this point (kernel pipe
    // + aplay's ALSA ring) is invisible to us and can only be estimated from
    // consumption deltas; see AudioPlayer::pumpAudio().
    qint64 pendingBytes();
    // Total bytes the pump has accepted via write() into the CURRENT child
    // since its spawn/last flushPipe(). Monotonic per child lifetime; reset
    // to 0 when the child is killed and respawned.
    qint64 handedOffBytes() const { return handedOff_; }
    // Bytes the kernel pipe has ACTUALLY delivered to the child's stdin so
    // far (QProcess::bytesWritten — NOT bytesToWrite, which is only our own
    // unsent queue). The push pump derives consumption from this counter:
    // bytes that left the pipe can only have gone into aplay's ALSA ring or
    // out of the speaker. Together with bytesInPipe() it makes the backlog
    // estimate exact for everything up to the pipe read end; only aplay's
    // own ring (~2 periods) remains an estimate.
    qint64 bytesIntoChild();
    // Exact unread byte count sitting in the KERNEL PIPE between us and the
    // child's stdin read end, measured with ioctl(FIONREAD) on the write-end
    // file descriptor (QProcess::ProcessChannel::StandardInput). This is the
    // quantity the old model could only bound: without it the pump assumed
    // the pipe was always full and credited the cursor with bytes merely
    // accepted by write() ("cursor sprints ahead of the sound at startup").
    // Returns 0 when the child is not running or the fd cannot be resolved.
    qint64 bytesInPipe();
    // intoChild minus the still-unread pipe content: bytes the child has
    // demonstrably pulled out of the pipe (into its ALSA ring / out of the
    // speaker). The push pump drives the audible cursor from this counter.
    qint64 auditedBytesIntoChild();
    // Exact number of bytes the child process currently has open on ITS read
    // end of our stdin pipe, resolved via /proc/<pid>/fd/0 -> pipe inode ->
    // matching fd in /proc/<child-pid>/fd/*. This is aplay's private handle:
    // flushPipe()/reapChild() close OUR write end, but if the child lingers
    // (blocked in snd_pcm_writei(), ignoring SIGTERM) it keeps draining the
    // old pipe through this fd — the "next play starts with leftover audio
    // from the file end" symptom. shutdown() closes it directly so stale PCM
    // can never survive an interrupt. Returns -1 when unresolvable.
    qint64 closeChildStdinReadEnd();
    // Zero the cumulative handoff counter: called whenever the child is
    // killed/respawned, so bytes in flight with the old process are not
    // counted against the fresh one.
    void resetHandoff();

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
    qint64 handedOff_ = 0;       // bytes written into the current child (pipe accounting)
    bool started_ = false;     // child was successfully spawned at least once
    bool finishing_ = false;   // EOF sent; deferred SIGTERM escalation armed
    bool flushedRecently_ = false; // last spawn followed flushPipe(): warm start
    // Cached write-end fd of the child's stdin pipe for FIONREAD occupancy
    // measurement (see bytesInPipe()); re-resolved after every respawn.
    int pipeFd_ = -1;
    bool pipeFdValid_ = false;
};

// Build the "aplay" raw-PCM argument list for a format. The volume parameter
// is vestigial (ignored): Ubuntu's alsa-utils is built --disable-softvol and
// its aplay has no gain flag; gain is applied in-process before the pipe.
QStringList aplayArgsFor(const QAudioFormat &fmt, double volume = 1.0);
// Create an opened write-only sink piping PCM to "aplay" (ALSA) or "ffplay".
QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent);
QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent);
