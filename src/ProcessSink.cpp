#include "ProcessSink.h"
#include "AudioPlayer.h"

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

ProcessSink::~ProcessSink() { shutdown(); }

void ProcessSink::restartWithArgs(const QStringList &newArgs) {
    args_ = newArgs;
    shutdown();
}

qint64 ProcessSink::readData(char *, qint64) { return 0; }

qint64 ProcessSink::writeData(const char *data, qint64 len) {
    if (!ensureStarted()) return -1;
    proc_->write(data, len);
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

void ProcessSink::close() { shutdown(); }

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
    return true;
}

void ProcessSink::shutdown() {
    if (!proc_) return;
    proc_->closeWriteChannel();
    proc_->waitForFinished(300);
    if (proc_->state() == QProcess::Running) {
        proc_->terminate();
        if (!proc_->waitForFinished(300)) proc_->kill();
    }
}

QStringList aplayArgsFor(const QAudioFormat &fmt, double volume) {
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
    // aplay applies its own hardware-independent gain; the in-process float
    // path multiplies by volume_ directly, so do the same here. 0 dB = full
    // scale; anything above 0 dB clips, so clamp to it.
    const double db = volume >= 0.999 ? 0.0
                     : 20.0 * std::log10(std::max(volume, 0.0005));
    args << "-v" << QString::number(db, 'f', 1);
    return args;
}

QIODevice *makeAplaySink(const QAudioFormat &fmt, QObject *parent) {
    // Spawn with the player's current volume (setVolumeApplier wires this up);
    // live slider changes respawn the process via AudioPlayer::
    // restartOutputKeepingPosition().
    const QString prog = resolveProgram(QStringLiteral("aplay"));
    if (prog.isEmpty()) return nullptr;   // alsa-utils not installed
    auto *ap = qobject_cast<AudioPlayer *>(parent);
    const double v = ap ? ap->volume() : 1.0;
    auto *s = new ProcessSink(prog, aplayArgsFor(fmt, v), parent);
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
    args << "-nodisp" << "-loglevel" << "quiet"
         << "-f" << sf
         << "-ar" << QString::number(fmt.sampleRate())
         << "-ac" << QString::number(fmt.channelCount())
         << "-i" << "-";
    auto *s = new ProcessSink(prog, args, parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}
