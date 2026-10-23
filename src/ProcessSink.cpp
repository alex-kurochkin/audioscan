#include "ProcessSink.h"
#include "AudioPlayer.h"

#include "AudioDebug.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
// FIONREAD on the child-stdin pipe fd (exact unread-pipe measurement, see
// ProcessSink::bytesInPipe()). POSIX only — this project targets Linux/Debian.
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>

namespace {
// Resolve an external player up front. QProcess::start() succeeds even when
// the binary doesn't exist (execvp failure surfaces asynchronously), which
// previously left us spawning a zombie "aplay" pipe and silently losing all
// audio. Returning an empty string lets the factory decline this sink so the
// next candidate (ffplay) gets used immediately.
QString resolveProgram(const QString &program) {
    const QString abs = QStandardPaths::findExecutable(program);
    if (!abs.isEmpty()) return abs;
    qWarning().noquote() << QStringLiteral("AudioPlayer: '%1' not found in PATH")
                                .arg(program);
    return QString();
}
} // namespace

ProcessSink::ProcessSink(const QString &program, const QStringList &args,
                         QObject *parent)
    : QIODevice(parent), program_(program), args_(args) {}

ProcessSink::~ProcessSink() {
    shutdown();
    // Reap synchronously: the deferred QTimer in shutdown() can no longer
    // run once this object is gone, so without this the child outlives its
    // QProcess ("QProcess: Destroyed while process ("/usr/bin/aplay") is
    // still running.") and an orphaned aplay keeps hogging the ALSA device.
    reapChild(2000);
}

void ProcessSink::reapChild(qint64 waitMs) {
    // Synchronously finish the child: close our write end (EOF), let aplay
    // drain the buffered PCM and exit on its own, then SIGTERM/SIGKILL as a
    // last resort. Called from the destructor AND from restartWithArgs():
    // without this, shutdown()'s deferred QTimer escalation never runs once
    // the object is gone, QProcess gets destroyed while the child still
    // lives ("QProcess: Destroyed while process ("/usr/bin/aplay") is still
    // running." + orphaned aplay holding the ALSA device), and restarting
    // the sink raced the old child for the device.
    if (!proc_) return;
    if (proc_->state() != QProcess::NotRunning) {
        proc_->closeWriteChannel();
        if (!proc_->waitForFinished(int(waitMs))) {
            // Escalation path only: the child ignored EOF. SIGTERM first —
            // it may surface as aplay's "write error: Interrupted system
            // call" (exitCode=1), which shutdown()/flushPipe() treat as a
            // benign unwind and do not log.
            proc_->terminate();
            if (!proc_->waitForFinished(1000)) proc_->kill();
            proc_->waitForFinished(1000);
        }
    }
    // The child is guaranteed NotRunning at this point. Destroying the
    // QProcess while its process still lives makes Qt print
    //   "QProcess: Destroyed while process (...) is still running."
    // straight to stderr as an INFO line on every sink restart (seek, stop,
    // track change) — previously spamming the user's logs and leaving
    // orphaned aplay instances fighting for the ALSA device. Reaping above
    // prevents that; delete here finalises the teardown deterministically.
    delete proc_;
    proc_ = nullptr;
    started_ = false;
}

void ProcessSink::restartWithArgs(const QStringList &newArgs) {
    args_ = newArgs;
    shutdown();
    // The next play() must not start a fresh child while the old one is
    // still draining the ALSA device — reap it here (blocking is safe:
    // this runs from UI code inside the event loop, not teardown).
    reapChild(2000);
}

qint64 ProcessSink::readData(char *, qint64) { return 0; }

qint64 ProcessSink::bytesIntoChild() {
    // Cumulative bytes the kernel has ACTUALLY pushed into the child's stdin
    // pipe. QProcess exposes no getter for this (only the bytesWritten signal
    // and the unsent bytesToWrite queue), so we derive it exactly:
    //     intoChild = acceptedByWrite(handedOff_) - stillInOurQueue(bytesToWrite)
    // Bytes that left our write buffer can only sit in the kernel pipe or in
    // aplay's hands — unlike the raw handedOff_, this counter stalls while
    // aplay is blocked opening the ALSA device and the pipe fills up, which
    // is precisely the startup signal the old wall-clock model was missing
    // ("cursor sprints ahead, sound starts late").
    if (!proc_) return 0;
    const qint64 queued = proc_->bytesToWrite();
    return std::max(qint64(0), handedOff_ - queued);
}

qint64 ProcessSink::bytesInPipe() {
    // EXACT unread byte count at the child's read end of the kernel stdin
    // pipe, measured with ioctl(FIONREAD). QProcess offers no API for this
    // (it owns only the write end's buffering), so we ask the kernel through
    // /proc: every fd of ours that is a `pipe:` inode whose `fd:` link target
    // is the child's read end IS our write end of that same pipe — FIONREAD
    // on it returns precisely the bytes buffered between the two ends. This
    // closes the blind spot that made every previous cursor model wrong:
    // without it the pump could not tell "aplay consumed my bytes" from "my
    // bytes merely filled the 64 KiB pipe while aplay is still opening the
    // device" — the cursor credited itself with the whole pipe depth at
    // startup (~0.7 s of S16 stereo @48k) and ran ahead of the sound.
    // Consumption is then exact:
    //     drained = intoChild - bytesInPipe()
    if (!pipeFdValid_) {
        if (!proc_ || proc_->state() != QProcess::Running) return 0;
        const qint64 pid = proc_->processId();
        if (pid <= 0) return 0;
        // Resolve the child's stdin target once per child lifetime.
        char link[64];
        std::snprintf(link, sizeof(link), "/proc/%lld/fd/0",
                      static_cast<long long>(pid));
        char childStdin[256];
        const ssize_t n = ::readlink(link, childStdin, sizeof(childStdin) - 1);
        if (n <= 0) return 0;
        childStdin[n] = '\0';
        // Find OUR fd that points at the same pipe inode.
        int found = -1;
        QDir fdDir(QStringLiteral("/proc/self/fd"));
        const QStringList fds = fdDir.entryList(QDir::NoDotAndDotDot | QDir::System);
        for (const QString &name : fds) {
            bool ok = false;
            const int fd = name.toInt(&ok);
            if (!ok || fd < 0) continue;
            char target[256];
            std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
            const ssize_t m = ::readlink(link, target, sizeof(target) - 1);
            if (m <= 0) continue;
            target[m] = '\0';
            if (std::strcmp(target, childStdin) == 0) { found = fd; break; }
        }
        if (found < 0) return 0;
        int probe = 0;
        if (ioctl(found, FIONREAD, &probe) < 0) return 0;
        pipeFd_ = found;
        pipeFdValid_ = true;
    }
    int n = 0;
    if (ioctl(pipeFd_, FIONREAD, &n) < 0) {
        pipeFdValid_ = false;   // fd died with the child: re-resolve next tick
        return 0;
    }
    return std::max(0, n);
}

qint64 ProcessSink::auditedBytesIntoChild() {
    // Bytes the child has DEMONSTRABLY removed from the pipe: everything it
    // ever received minus what is still sitting there unread. Monotonic by
    // construction within one child lifetime (intoChild grows, pipe drains).
    return std::max(qint64(0), bytesIntoChild() - bytesInPipe());
}

qint64 ProcessSink::closeChildStdinReadEnd() {
    // Nuke the pipe contents from the CHILD side. Closing our write end only
    // signals EOF; a lingering aplay (blocked in snd_pcm_writei(), ignoring
    // SIGTERM — exactly what "QProcess: Destroyed while process is still
    // running" reported) keeps reading stale PCM through its own fd, and the
    // next session then starts with leftover audio from the old position.
    // Measure how many bytes are still queued at the child's read end (via
    // /proc/<pid>/fd/0 -> pipe inode -> matching fd), so callers can log it;
    // actual destruction of those bytes happens when the caller kills the
    // child below (the kernel frees the pipe once the last reader dies).
    // Returns the unread byte count, or -1 when unresolvable.
    if (!proc_ || proc_->state() == QProcess::NotRunning) return -1;
    const qint64 pid = proc_->processId();
    if (pid <= 0) return -1;
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/%lld/fd/0", static_cast<long long>(pid));
    char childStdin[256];
    const ssize_t n = ::readlink(link, childStdin, sizeof(childStdin) - 1);
    if (n <= 0) return -1;
    childStdin[n] = '\0';
    if (std::strncmp(childStdin, "pipe:[", 6) != 0) return -1;
    int found = -1;
    char dir[64];
    std::snprintf(dir, sizeof(dir), "/proc/%lld/fd", static_cast<long long>(pid));
    QDir fdDir(QString::fromLocal8Bit(dir));
    const QStringList fds = fdDir.entryList(QDir::NoDotAndDotDot | QDir::System);
    for (const QString &name : fds) {
        bool ok = false;
        const int fd = name.toInt(&ok);
        if (!ok || fd < 0) continue;
        char target[256];
        std::snprintf(link, sizeof(link), "/proc/%lld/fd/%d", static_cast<long long>(pid), fd);
        const ssize_t m = ::readlink(link, target, sizeof(target) - 1);
        if (m <= 0) continue;
        target[m] = '\0';
        if (std::strcmp(target, childStdin) == 0) { found = fd; break; }
    }
    if (found < 0) return -1;
    // Open the child's fd path read-only: this gives US a handle on the same
    // pipe to query FIONREAD without disturbing the child. We close it again
    // immediately — the pipe itself dies when the child is killed.
    char fdPath[96];
    std::snprintf(fdPath, sizeof(fdPath), "/proc/%lld/fd/%d", static_cast<long long>(pid), found);
    const int ours = ::open(fdPath, O_RDONLY | O_NONBLOCK);
    if (ours < 0) return -1;
    int pending = 0;
    if (ioctl(ours, FIONREAD, &pending) < 0) pending = -1;
    ::close(ours);
    return pending;
}

qint64 ProcessSink::pendingBytes() {
    // Bytes still queued at OUR side: QProcess's write buffer holds data
    // accepted by write() but not yet handed to the kernel pipe. This is
    // NON-destructive bookkeeping — the pump calls it every tick, so it must
    // NOT touch handedOff_ (the old discardBuffered() estimate conflated the
    // two counters and could never see the child consuming anything once the
    // kernel pipe filled up).
    if (!proc_) return 0;
    return proc_->bytesToWrite();
}

void ProcessSink::resetHandoff() {
    handedOff_ = 0;
}

void ProcessSink::flushPipe(bool force) {
    // Drop everything the pump has already handed to the child's stdin pipe.
    // A seek must be heard at once: without this, QProcess's internal buffer
    // plus the kernel pipe (typically ~64 KiB ≈ 0.37 s of S16 stereo @44.1k)
    // keep feeding stale PCM from the OLD position into aplay even after the
    // player reset posFrames_ — exactly the "cursor moved but the sound
    // catches up slowly / Stop keeps playing for a while" symptom.
    if (!proc_) return;
    const qint64 queued = proc_->bytesToWrite();
    flushedRecently_ = true;             // next spawn is "warm": skip stderr wait
    DBG(QStringLiteral("AudioPlayer: flushing sink '%1' pipe (%2 bytes queued)"
                       " — stale audio dropped")
            .arg(program_).arg(queued));
    if (proc_->state() == QProcess::Running) {
        // Graceful EOF only makes sense when there is nothing left to drop:
        // aplay then finishes draining and exits 0. But on a *forced* flush
        // (click-to-seek / stop with an active session) the child may be
        // blocked in snd_pcm_writei() with the kernel pipe + its ALSA ring
        // still holding up to ~0.5 s of audio from the OLD position. Waiting
        // for that drain here (waitForFinished(500) + synchronous reap) is
        // what made every click freeze the UI for half a second before the
        // cursor jumped. SIGINT interrupts aplay immediately (the resulting
        // "Interrupted system call" unwind is treated as benign in
        // shutdown()), so the respawn starts from the new position at once.
        if (queued == 0 && !force) {
            proc_->closeWriteChannel();  // EOF: aplay drains and exits cleanly
            if (!proc_->waitForFinished(500))
                proc_->terminate();      // escalation only if it lingers
        } else {
            proc_->terminate();          // instant interrupt, no drain wait
            proc_->waitForFinished(150);
            if (proc_->state() == QProcess::Running) proc_->kill();
        }
    }
    delete proc_;                        // reap synchronously; no orphan
    proc_ = nullptr;
    started_ = false;                    // ensureStarted() respawns lazily
    pipeFdValid_ = false;                // fd died with the child
    // Everything in flight (QProcess queue + kernel pipe) died with the
    // child: restart the handoff accounting so the fresh process starts
    // from an empty backlog.
    handedOff_ = 0;
}

qint64 ProcessSink::writeData(const char *data, qint64 len) {
    if (!ensureStarted()) return -1;
    // A child that is blocked in snd_pcm_writei() (device underrun/busy) can
    // leave our side of the pipe full: then write() returns 0 bytes and
    // waitForBytesWritten() times out. Returning 0 (not len!) keeps
    // QIODevice's buffered writer from discarding the rest of the chunk —
    // the caller simply retries the same bytes on the next pump tick.
    const qint64 written = proc_->write(data, len);
    if (written <= 0) {
        if (proc_->state() == QProcess::NotRunning) {
            qWarning().noquote() << QStringLiteral("AudioPlayer: sink '%1' died (exitCode=%2)")
                                        .arg(program_).arg(proc_->exitCode());
            return -1;
        }
        qWarning().noquote() << QStringLiteral("AudioPlayer: sink '%1' pipe stalled"
                                              " (write returned %2, buffer=%3)")
                                    .arg(program_)
                                    .arg(written)
                                    .arg(proc_->bytesToWrite());
        return 0;   // transient back-pressure: retry later with the same data
    }
    handedOff_ += written;   // backlog accounting for the push pump's pacing cap
    proc_->waitForBytesWritten(500);
    // A dead child (e.g. "aplay: not found" — QProcess::start() succeeds even
    // when execvp fails; the failure only surfaces asynchronously as
    // FailedToStart) must be reported so AudioPlayer can drop to the next
    // sink instead of silently pumping bytes into a zombie process.
    if (proc_->state() == QProcess::NotRunning) {
        qWarning().noquote() << QStringLiteral("AudioPlayer: sink '%1' died (exitCode=%2)")
                                    .arg(program_).arg(proc_->exitCode());
        return -1;
    }
    return len;
}

void ProcessSink::close() {
    // Graceful EOF: close only our write channel so the child drains whatever
    // is already buffered in the pipe and exits 0 on its own (that is how a
    // healthy end-of-playback looks to aplay). We must NOT terminate()/kill()
    // here: SIGTERM/SIGKILL interrupts aplay's blocking snd_pcm_writei() and
    // produces the bogus "write error: Interrupted system call" + exitCode=1
    // that previously showed up as an alarming FATAL log line after every
    // stop/pause. The process itself is reaped later in shutdown() (destructor
    // or the next play session), which by then sees a clean NormalExit/0.
    if (!proc_) return;
    if (proc_->state() == QProcess::Running) proc_->closeWriteChannel();
}

bool ProcessSink::ensureStarted() {
    if (proc_) {
        if (proc_->state() == QProcess::NotRunning &&
            proc_->exitStatus() != QProcess::NormalExit)
            return false;   // crashed earlier — don't retry
        return proc_->state() == QProcess::Running;
    }
    proc_ = new QProcess(this);
    proc_->setProgram(program_);
    proc_->setArguments(args_);
    pipeFdValid_ = false;   // fresh child: re-resolve the stdin pipe fd lazily
    // Keep stderr separate (do NOT use MergedChannels): anything the child
    // writes to stderr ("aplay: sound.c:341: failed reason..." etc.) lands in
    // the readyReadStandardError buffer and is logged on failure below.
    // stdout is discarded so routine child chatter (libmpg123 warnings on
    // broken MP3s) doesn't spam the terminal.
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    proc_->setStandardOutputFile(QStringLiteral("/dev/null"));
    // Log the exact command line once: when a child misbehaves (bad option,
    // device busy, wrong format) this is the first thing to reproduce by hand.
    // NOTE: DBG() (not qInfo/qWarning) on purpose — those stream operators
    // are captured by our global message handler and stamped as [INFO]/
    // [WARNING] regardless of the level we intend; DBG always lands as
    // [DEBUG]. ("spawning sink ... running" lines previously showed up as
    // scary [FATAL] in the user's logs for perfectly healthy startups.)
    DBG(QStringLiteral("AudioPlayer: spawning sink '%1 %2'")
            .arg(program_, args_.join(QLatin1Char(' '))));
    proc_->start();
    // waitForStarted() only confirms our fork/exec helper ran; an ENOENT on
    // the real binary arrives one event-loop turn later as FailedToStart.
    // Give that signal a chance to arrive before declaring success.
    if (!proc_->waitForStarted(3000)) {
        const QString err = proc_->errorString();
        const QByteArray se = proc_->readAllStandardError();
        qWarning().noquote()
            << QStringLiteral("AudioPlayer: failed to start '%1': %2%3")
                   .arg(program_, err,
                        se.isEmpty() ? QString()
                                     : QStringLiteral(" | stderr: %1")
                                           .arg(QString::fromUtf8(se).trimmed()));
        delete proc_;
        proc_ = nullptr;
        return false;
    }
    // Drain any early stderr the child produced while opening the device and
    // echo it once — this is usually the exact ALSA error we're debugging.
    // The 150 ms wait only applies to a *cold* spawn (first start of the
    // session). After flushPipe() (seek/stop) the respawn must be immediate:
    // waiting here on every seek would add audible latency, and a warm aplay
    // that already opened the device successfully has nothing new to say.
    if (!flushedRecently_) {
        proc_->waitForReadyRead(150);
    } else {
        flushedRecently_ = false;
        proc_->waitForStarted(100);
    }
    {
        const QByteArray se = proc_->readAllStandardError();
        if (!se.trimmed().isEmpty())
            qWarning().noquote()
                << QStringLiteral("AudioPlayer: sink '%1' stderr: %2")
                       .arg(program_, QString::fromUtf8(se).trimmed());
    }
    DBG(QStringLiteral("AudioPlayer: sink '%1' running (pid=%2)")
            .arg(program_).arg(proc_->processId()));
    started_ = true;
    return true;
}

void ProcessSink::shutdown() {
    if (!proc_) return;
    // If the child exited on its own before we tore it down, surface why —
    // otherwise a mid-stream death looks like "playback silently stopped".
    if (proc_->state() == QProcess::NotRunning && started_) {
        const QByteArray se = proc_->readAllStandardError();
        // Closing our write channel is how a healthy run ends: aplay sees EOF
        // and exits 0. Two benign cases must NOT be logged as warnings:
        //  * clean exit (code 0, no stderr);
        //  * the -EINTR unwind after closeWriteChannel(): ALSA's blocking
        //    snd_pcm_writei() returns EINTR when the pipe's write end closes,
        //    so aplay prints "write error: Interrupted system call" and exits
        //    1. That is our own graceful shutdown signature, not a failure.
        const bool eintrUnwind =
            se.contains("Interrupted system call") || se.contains("EINTR");
        const bool noteworthy =
            !eintrUnwind &&
            (se.trimmed().isEmpty()
                ? (proc_->exitStatus() == QProcess::CrashExit ||
                   proc_->exitCode() != 0)
                : true);
        if (noteworthy)
            qWarning().noquote()
                << QStringLiteral("AudioPlayer: sink '%1' already exited"
                                  " (exitCode=%2, status=%3)%4")
                       .arg(program_)
                       .arg(proc_->exitCode())
                       .arg(proc_->exitStatus() == QProcess::CrashExit
                                ? QStringLiteral("CrashExit")
                                : QStringLiteral("NormalExit"))
                       .arg(se.trimmed().isEmpty()
                                ? QString()
                                : QStringLiteral(" | stderr: %1").arg(
                                      QString::fromUtf8(se).trimmed()));
    }
    proc_->closeWriteChannel();
    // Give aplay time to drain the remaining pipe bytes through ALSA and exit
    // cleanly on its own. The wait happens in the event loop via a queued
    // re-check (shutdown() may be called from a destructor during teardown,
    // when blocking here would be unsafe anyway), then we only escalate to
    // SIGTERM if the child is *still* alive — which after EOF basically never
    // happens, so healthy stop/seek no longer interrupts snd_pcm_writei().
    //
    // HARD DEADLINE ESCALATION: the old code waited 1500 ms before even
    // SIGTERM-ing, and aplay blocked in snd_pcm_writei() on a busy device can
    // sit inside that syscall far longer (the user's log shows "QProcess:
    // Destroyed while process is still running" — the deferred escalation
    // timer never fired before the object died). An orphaned aplay keeps the
    // kernel pipe + ALSA ring draining stale PCM: exactly why the NEXT play
    // started with leftover audio from the end of the previous file. So the
    // escalation now runs in THREE synchronous steps with short budgets:
    // EOF -> SIGINT (aplay's own handler unwinds the EINTR write) -> SIGKILL.
    // After kill(), the last reader of the pipe dies and the kernel discards
    // every unread byte — no stale audio can survive this function.
    if (!finishing_) {
        finishing_ = true;
        if (proc_->state() == QProcess::Running) {
            // Step 1: EOF drain window, bounded and SHORT. waitForFinished
            // processes events internally, so the UI stays responsive.
            if (!proc_->waitForFinished(300)) {
                // Step 2: SIGINT — aplay catches it and exits promptly even
                // mid-writei (this is what flushPipe()'s forced path does).
                proc_->terminate();
                if (!proc_->waitForFinished(300)) {
                    // Step 3: unconditional kill. Log how much stale audio we
                    // are destroying (measured at the child's read end).
                    const qint64 stale = closeChildStdinReadEnd();
                    DBG(QStringLiteral("AudioPlayer: sink '%1' ignored EOF/SIGINT;"
                                       " killed (pid=%2, %3 stale bytes in pipe "
                                       "discarded)")
                            .arg(program_)
                            .arg(proc_->processId())
                            .arg(stale));
                    proc_->kill();
                    proc_->waitForFinished(500);
                }
            }
        }
    }
}

QStringList aplayArgsFor(const QAudioFormat &fmt, double /*volume*/) {
    auto tag = [](QAudioFormat::SampleFormat sf) -> QString {
        switch (sf) {
        case QAudioFormat::Int16: return QStringLiteral("S16_LE");
        case QAudioFormat::Int32: return QStringLiteral("S32_LE");
        case QAudioFormat::Float: return QStringLiteral("F32_LE");
        case QAudioFormat::UInt8: return QStringLiteral("U8");
        default:                  return QStringLiteral("S16_LE");
        }
    };
    QStringList args;
    args << "-q" << "-t" << "raw"
         << "-c" << QString::number(fmt.channelCount())
         << "-r" << QString::number(fmt.sampleRate())
         << "-f" << tag(fmt.sampleFormat());
    // NOTE: no volume flag here on purpose. Debian/Ubuntu ship alsa-utils
    // patched with --disable-softvol, so their aplay has neither -V nor -v
    // ("aplay: invalid option -- '1'" was exactly that: our dB value like
    // "-6.0" reached getopt and '-6'/'-1' were parsed as bogus short
    // options). The gain is applied in-process instead — generateBytes()
    // multiplies every PCM chunk by the player's atomic volume before it
    // reaches the pipe — so aplay runs at unity gain and the slider keeps
    // working live without respawning the child.
    return args;
}

QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent) {
    const QString prog = resolveProgram(QStringLiteral("aplay"));
    if (prog.isEmpty()) return nullptr;   // alsa-utils not installed
    auto *s = new ProcessSink(prog, aplayArgsFor(fmt, 1.0), parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}

QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent) {
    const QString prog = resolveProgram(QStringLiteral("ffplay"));
    if (prog.isEmpty()) return nullptr;   // ffmpeg not installed
    QStringList args;
    // NOTE: ffplay has no -autoexit option; it exits on EOF of stdin input.
    // The pump always feeds signed 16-bit PCM (see AudioPlayer::play()), so
    // declare s16le — f32le here turned the stream into garbage noise.
    const QString sf = fmt.sampleFormat() == QAudioFormat::Float
                           ? QStringLiteral("f32le")
                           : QStringLiteral("s16le");
    // Volume is applied in-process by AudioPlayer::generateBytes() before
    // the bytes reach this pipe, so ffplay needs no gain handling of its
    // own and live slider changes need no respawn.
    args << "-nodisp" << "-loglevel" << "quiet"
         << "-f" << sf
         << "-ar" << QString::number(fmt.sampleRate())
         << "-ac" << QString::number(fmt.channelCount())
         << "-i" << "-";
    auto *s = new ProcessSink(prog, args, parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}
