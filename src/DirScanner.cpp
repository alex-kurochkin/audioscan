#include "DirScanner.h"
#include "AudioFile.h"
#include "Types.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QLoggingCategory>

DirScanner::DirScanner(QObject *parent) : QObject(parent) {}

void DirScanner::scan(const QString &dir) {
    requestedDir_ = dir;
    running_ = true;
    // Background scan (spec 3.1/12): never block UI.
    auto future = QtConcurrent::run([dir]() {
        QVector<FileTableModel::Row> rows;
        QDir d(dir);
        const auto entries = d.entryInfoList(QDir::Files, QDir::Name);
        for (const QFileInfo &fi : entries) {
            if (!isAudioFile(fi.absoluteFilePath().toLower())) continue;
            FileTableModel::Row r;
            r.path = fi.absoluteFilePath();
            r.name = fi.fileName();
            r.size = fi.size();
            // birthTime is often invalid on Linux filesystems; fall back to ctime.
            r.created = fi.birthTime().isValid() ? fi.birthTime() : fi.metadataChangeTime();
            if (!r.created.isValid()) r.created = fi.fileTime(QFileDevice::FileBirthTime);
            r.modified = fi.lastModified();
            AudioData meta;
            QString err;
            if (AudioFile::probe(r.path, meta, &err) && meta.valid) {
                r.broken = false;
                r.sampleRate = meta.sampleRate;
                r.channels = meta.channels;
                r.frames = meta.totalFrames;
                r.sampleTypeName = meta.format == SampleFormat::Unknown
                    ? QStringLiteral("compressed") : sampleFormatName(meta.format);
                r.comment = meta.comment;
            } else {
                qWarning() << "scanDirectory: probe failed for" << r.path << "-" << err;
                r.broken = true;   // red row, no params, no preview (spec 3.3)
            }
            rows.push_back(r);
        }
        return rows;
    });
    // poll-free completion via watcher
    auto *watcher = new QFutureWatcher<QVector<FileTableModel::Row>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, dir]() {
        const QString target = requestedDir_;
        if (target == dir) running_ = false;
        watcher->deleteLater();
        if (target != dir) return;   // superseded scan: drop silently
        emit finished(DirScanResult{dir, watcher->result()});
    });
    watcher->setFuture(future);
}
