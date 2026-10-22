#pragma once
#include <QSqlDatabase>
#include <QVector>
#include "Types.h"

// SQLite storage: markers, preview cache (UUID <-> file), last session state.
class Database {
public:
    static Database &instance();

    bool open(const QString &path);

    // ---- Markers ----
    QVector<Marker> loadMarkers(const QString &filePath);
    void saveMarkers(const QString &filePath, qint64 fileSize, qint64 mtimeMs,
                     const QVector<Marker> &markers);
    void deleteMarkersForFile(const QString &filePath);
    void clearAllMarkers();
    // Scan algorithm (spec 4.5): remove entries whose file is missing or size/mtime mismatch.
    int scanMarkers();
    void renameFilePath(const QString &oldPath, const QString &newPath);

    // ---- Preview cache ----
    // Key = path + size + mtime. Returns UUID of cached image if valid.
    QString lookupPreview(const QString &path, qint64 size, qint64 mtimeMs, const QString &mode);
    void storePreview(const QString &path, qint64 size, qint64 mtimeMs,
                      const QString &mode, const QString &uuid);
    void invalidatePreviewsForDir(const QString &dirPath);
    void clearAllPreviews();

    // ---- Session state ----
    void setSessionState(const QString &dir, const QString &file, double positionSec);
    void getSessionState(QString &dir, QString &file, double &positionSec);

    // ---- Generic key/value session entries (column widths, ...) ----
    void setSessionValue(const QString &key, const QString &value);
    QString sessionValue(const QString &key);

private:
    Database() = default;
    QSqlDatabase db_;
};
