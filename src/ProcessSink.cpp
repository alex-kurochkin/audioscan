#include "ProcessSink.h"
#include "AudioPlayer.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

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
    // ForwardedErrorChannel would spam the terminal with child chatter
    // (e.g. libmpg123 warnings on broken MP3s). Discard stderr; failures
    // are still surfaced via "failed to start" above and exit-code checks.
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    proc_->setStandardOutputFile(QStringLiteral("/dev/null"));
    proc_->start();
    if (!proc_->waitForStarted(3000)) {
        qWarning().noquote()
            << QStringLiteral("AudioPlayer: failed to start '%1'").arg(program_);
        delete proc_;
        proc_ = nullptr;
        return false;
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
    auto *ap = qobject_cast<AudioPlayer *>(parent);
    const double v = ap ? ap->volume() : 1.0;
    auto *s = new ProcessSink(QStringLiteral("aplay"), aplayArgsFor(fmt, v), parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}

QIODevice *makeFfplaySink(const QAudioFormat &fmt, QObject *parent) {
    QStringList args;
    // NOTE: ffplay has no -autoexit option; it exits on EOF of stdin input.
    args << "-nodisp" << "-loglevel" << "quiet"
         << "-f" << "f32le"
         << "-ar" << QString::number(fmt.sampleRate())
         << "-ac" << QString::number(fmt.channelCount())
         << "-i" << "-";
    auto *s = new ProcessSink(QStringLiteral("ffplay"), args, parent);
    s->open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    return s;
}
