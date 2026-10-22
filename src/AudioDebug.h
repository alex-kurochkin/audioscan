#pragma once
#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <QStringConverter>
#include <QTextStream>

#include <cstdio>  // fputs / fflush for the stderr mirror

// ---------------- error logging (always on, opt-out) ----------------
// Every playback-relevant failure (qWarning/qCritical and our own DBG()
// diagnostics) is appended to a per-day log file under the application's
// config directory (~/.config/audioscout/logs/) AND mirrored to stderr.
// The file is opened in append mode per line, so several instances can
// write concurrently without clobbering each other, and nothing is lost
// when the app crashes. Set AUDIOSCOUT_LOG=0 to disable the file sink
// (stderr mirroring stays on). Any internal logging failure is swallowed
// silently — diagnostics must never break the app itself.
namespace audioscout_log {

inline QString logDirPath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config/audioscout");
    return base + QStringLiteral("/logs");
}

inline QString logFilePath() {
    return logDirPath() + QStringLiteral("/audioscout-%1.log")
        .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
}

inline bool fileSinkEnabled() {
    static const bool off = qEnvironmentVariableIsSet("AUDIOSCOUT_LOG") &&
                            qEnvironmentVariable("AUDIOSCOUT_LOG") == "0";
    return !off;
}

inline void writeLine(const QString &line) {
    // Mirror to stderr first: it works even if the file sink fails.
    fputs(qPrintable(line + QLatin1Char('\n')), stderr);
    fflush(stderr);
    if (!fileSinkEnabled()) return;
    static QMutex mtx;
    QMutexLocker lock(&mtx);
    QDir().mkpath(logDirPath());   // idempotent; ignored on failure
    QFile f(logFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << line << '\n';
}

// Unified message handler: routes every Qt log record (qWarning, qCritical,
// and qDebug from DBG()) into the log file + stderr with a timestamp.
inline QtMessageHandler gPrevHandler = nullptr;

inline void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    static const char *tag[] = {"DEBUG", "INFO", "WARNING", "CRITICAL", "FATAL"};
    // NOTE: we used to demote messages containing "spawning sink"/"running
    // (pid=" from FATAL to INFO here. That was a band-aid: the real problem
    // was that healthy lifecycle chatter was logged through qInfo(), which
    // this handler stamps with whatever level Qt reports — and on some Qt
    // builds qInfo records arrived mislabeled. The spawn/pid lines now use
    // DBG() directly, so no string sniffing is needed in the handler.
    QtMsgType t = type;
    const int lvl = qMin(int(t), 4);
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    QString line = QStringLiteral("%1 [%2] %3").arg(stamp, QLatin1String(tag[lvl]), msg);
    if (ctx.file && ctx.line >= 0)
        line += QStringLiteral("  (%1:%2)").arg(QLatin1String(ctx.file)).arg(ctx.line);
    writeLine(line);
}

inline void install() {
    static bool done = false;
    if (done) return;
    done = true;
    gPrevHandler = qInstallMessageHandler(handler);
}

}  // namespace audioscout_log

// ---------------- debug logging (opt-in, off by default) ----------------
// AUDIOSCOUT_DEBUG=1 routes DBG() messages through qDebug() as before.
// In addition, every DBG() call now ALWAYS lands in the always-on error log
// file (+stderr), so playback diagnostics are captured without the user
// having to know about the env switch — this is what makes "Play does
// nothing" bugs diagnosable after the fact.
inline bool audioScoutDebugEnabled() {
    return qEnvironmentVariableIsSet("AUDIOSCOUT_DEBUG");
}

#define DBG(msg) do { \
    const QString asDbgMsg_ = (msg); \
    audioscout_log::writeLine(QStringLiteral("%1 [DEBUG] [audioscout] %2") \
        .arg(QDateTime::currentDateTime().toString( \
                 QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")), asDbgMsg_)); \
    if (audioScoutDebugEnabled()) \
        qDebug().noquote() << "[audioscout]" << asDbgMsg_; \
} while (0)
