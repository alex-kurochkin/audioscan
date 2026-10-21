#include "Database.h"
#include <QSqlQuery>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

static const int kFormatVersion = 1;

Database &Database::instance() {
    static Database inst;
    return inst;
}

bool Database::open(const QString &path) {
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    db_.setDatabaseName(path);
    if (!db_.open()) return false;
    QSqlQuery q(db_);
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS markers("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "path TEXT NOT NULL, size INTEGER, mtime INTEGER,"
        "data TEXT NOT NULL)"));   // JSON array of {name,time_samples,time_sec}
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS previews("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "path TEXT, size INTEGER, mtime INTEGER, mode TEXT,"
        "uuid TEXT, format_version INTEGER)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS session("
        "key TEXT PRIMARY KEY, value TEXT)"));
    return true;
}

// ---------- Markers ----------

QVector<Marker> Database::loadMarkers(const QString &filePath) {
    QVector<Marker> out;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT id, data FROM markers WHERE path=? ORDER BY id"));
    q.addBindValue(filePath);
    if (!q.exec()) return out;
    while (q.next()) {
        qint64 id = q.value(0).toLongLong();
        QJsonArray arr = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).array();
        for (const auto &v : arr) {
            QJsonObject o = v.toObject();
            Marker m;
            m.id = id; // one DB row per file; ids kept coarse
            m.name = o.value(QStringLiteral("name")).toString();
            m.timeSamples = qint64(o.value(QStringLiteral("time_samples")).toDouble());
            m.timeSec = o.value(QStringLiteral("time_sec")).toDouble();
            out.push_back(m);
        }
    }
    return out;
}

void Database::saveMarkers(const QString &filePath, qint64 fileSize, qint64 mtimeMs,
                           const QVector<Marker> &markers) {
    QSqlQuery q(db_);
    q.exec(QStringLiteral("BEGIN"));
    q.prepare(QStringLiteral("DELETE FROM markers WHERE path=?"));
    q.addBindValue(filePath);
    q.exec();
    if (!markers.isEmpty()) {
        QJsonArray arr;
        for (const auto &m : markers) {
            QJsonObject o;
            o[QStringLiteral("name")] = m.name;
            o[QStringLiteral("time_samples")] = double(m.timeSamples);
            o[QStringLiteral("time_sec")] = m.timeSec;
            arr.append(o);
        }
        q.prepare(QStringLiteral(
            "INSERT INTO markers(path,size,mtime,data) VALUES(?,?,?,?)"));
        q.addBindValue(filePath);
        q.addBindValue(fileSize);
        q.addBindValue(mtimeMs);
        q.addBindValue(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        q.exec();
    }
    q.exec(QStringLiteral("COMMIT"));
}

void Database::deleteMarkersForFile(const QString &filePath) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM markers WHERE path=?"));
    q.addBindValue(filePath);
    q.exec();
}

void Database::clearAllMarkers() {
    QSqlQuery(db_).exec(QStringLiteral("DELETE FROM markers"));
}

int Database::scanMarkers() {
    int removed = 0;
    QSqlQuery q(db_);
    q.exec(QStringLiteral("SELECT id, path, size, mtime FROM markers"));
    QList<int> ids;
    while (q.next()) {
        int id = q.value(0).toInt();
        QString path = q.value(1).toString();
        qint64 size = q.value(2).toLongLong();
        qint64 mtime = q.value(3).toLongLong();
        QFileInfo fi(path);
        bool ok = fi.exists() && fi.size() == size &&
                  fi.lastModified().toMSecsSinceEpoch() == mtime;
        if (!ok) ids.push_back(id);
    }
    QSqlQuery del(db_);
    del.prepare(QStringLiteral("DELETE FROM markers WHERE id=?"));
    for (int id : ids) {
        del.addBindValue(id);
        del.exec();
        ++removed;
    }
    return removed;
}

void Database::renameFilePath(const QString &oldPath, const QString &newPath) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE markers SET path=? WHERE path=?"));
    q.addBindValue(newPath);
    q.addBindValue(oldPath);
    q.exec();
    q.prepare(QStringLiteral("UPDATE previews SET path=? WHERE path=?"));
    q.addBindValue(newPath);
    q.addBindValue(oldPath);
    q.exec();
}

// ---------- Preview cache ----------

QString Database::lookupPreview(const QString &path, qint64 size, qint64 mtimeMs,
                                const QString &mode) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT uuid FROM previews WHERE path=? AND size=? AND mtime=? AND mode=? AND format_version=?"));
    q.addBindValue(path);
    q.addBindValue(size);
    q.addBindValue(mtimeMs);
    q.addBindValue(mode);
    q.addBindValue(kFormatVersion);
    if (q.exec() && q.next()) {
        QString uuid = q.value(0).toString();
        return uuid;
    }
    return QString();
}

void Database::storePreview(const QString &path, qint64 size, qint64 mtimeMs,
                            const QString &mode, const QString &uuid) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "DELETE FROM previews WHERE path=? AND size=? AND mtime=? AND mode=?"));
    q.addBindValue(path); q.addBindValue(size); q.addBindValue(mtimeMs); q.addBindValue(mode);
    q.exec();
    q.prepare(QStringLiteral(
        "INSERT INTO previews(path,size,mtime,mode,uuid,format_version) VALUES(?,?,?,?,?,?)"));
    q.addBindValue(path); q.addBindValue(size); q.addBindValue(mtimeMs);
    q.addBindValue(mode); q.addBindValue(uuid); q.addBindValue(kFormatVersion);
    q.exec();
}

void Database::invalidatePreviewsForDir(const QString &dirPath) {
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM previews WHERE path LIKE ?"));
    q.addBindValue(dirPath + QStringLiteral("/%"));
    q.exec();
}

void Database::clearAllPreviews() {
    QSqlQuery(db_).exec(QStringLiteral("DELETE FROM previews"));
}

// ---------- Session state ----------

void Database::setSessionState(const QString &dir, const QString &file, double positionSec) {
    QSqlQuery q(db_);
    auto set = [&](const QString &k, const QString &v) {
        q.prepare(QStringLiteral(
            "INSERT INTO session(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
        q.addBindValue(k); q.addBindValue(v); q.exec();
    };
    set(QStringLiteral("lastDir"), dir);
    set(QStringLiteral("lastFile"), file);
    set(QStringLiteral("lastPos"), QString::number(positionSec));
}

void Database::getSessionState(QString &dir, QString &file, double &positionSec) {
    QSqlQuery q(db_);
    q.exec(QStringLiteral("SELECT key, value FROM session"));
    while (q.next()) {
        QString k = q.value(0).toString();
        if (k == QLatin1String("lastDir")) dir = q.value(1).toString();
        else if (k == QLatin1String("lastFile")) file = q.value(1).toString();
        else if (k == QLatin1String("lastPos")) positionSec = q.value(1).toDouble();
    }
}
