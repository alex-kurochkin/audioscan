#include "ProcessSink.h"
#include "AudioPlayer.h"

#include "AudioDebug.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

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
            proc_->terminate();
            if (!proc_->waitForFinished(1000)) proc_->kill();
            proc_->waitForFinished(1000);
        }
    }
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
    {
        proc_->waitForReadyRead(150);
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
    if (!finishing_) {
        finishing_ = true;
        QPointer<ProcessSink> self(this);
        QTimer::singleShot(1500, this, [self] {
            if (!self || !self->proc_) return;
            if (self->proc_->state() == QProcess::Running) {
                self->proc_->terminate();
                if (!self->proc_->waitForFinished(2000)) self->proc_->kill();
            }
        });
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
