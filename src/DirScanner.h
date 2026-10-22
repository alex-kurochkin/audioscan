#pragma once
#include <QObject>
#include <QVector>
#include "FileTableModel.h"

// Result of a background directory scan (used with queued signal delivery).
struct DirScanResult {
    QString dir;
    QVector<FileTableModel::Row> rows;
};
Q_DECLARE_METATYPE(DirScanResult)

// Background (non-blocking, spec 3.1/12) scan of one directory: collects the
// audio files with their metadata and marks unreadable ones as broken. Emits
// finished() on the GUI thread; results for superseded scans are dropped by
// the caller (or automatically, when the scanned dir differs from the
// currently requested one).
class DirScanner : public QObject {
    Q_OBJECT
public:
    explicit DirScanner(QObject *parent = nullptr);
    void scan(const QString &dir);          // starts a new scan (replaces pending)
    bool running() const { return running_; }

signals:
    void finished(DirScanResult result);

private:
    QString requestedDir_;
    bool running_ = false;
};
