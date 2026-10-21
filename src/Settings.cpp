#include "Settings.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>

static const QMap<QString, QString> kDefaultHotkeys = {
    {"mute",            "Ctrl+M"},
    {"normalize",       "Ctrl+N"},
    {"prev",            "P"},
    {"next",            "N"},
    {"playpause",       "Space"},
    {"gotoStart",       "W"},
    {"undo",            "Ctrl+Z"},
    {"redo",            "Ctrl+Shift+Z"},
    {"zoomIn",          "Ctrl+Up"},
    {"zoomOut",         "Ctrl+Down"},
    {"zoomReset",       "Ctrl+0"},
    {"addMarker",       "Alt+M"},
    {"deleteMarker",    "Delete"},
    {"clearSelection",  "Esc"},
};

Settings &Settings::instance() {
    static Settings inst;
    return inst;
}

Settings::Settings() {
    filePath_ = configDir() + QStringLiteral("/config.json");
    load();
}

QVariant Settings::value(const QString &key, const QVariant &fallback) const {
    if (data_.contains(key)) return data_.value(key).toVariant();
    return fallback;
}

void Settings::setValue(const QString &key, const QVariant &v) {
    data_[key] = QJsonValue::fromVariant(v);
    save();
}

void Settings::removeKey(const QString &key) {
    data_.remove(key);
    save();
}

void Settings::load() {
    QFile f(filePath_);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject())
            data_ = doc.object();
    }
}

void Settings::save() {
    QDir().mkpath(QFileInfo(filePath_).absolutePath());
    QFile f(filePath_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(data_).toJson(QJsonDocument::Indented));
}

QString Settings::configDir() const {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (d.isEmpty()) d = QDir::homePath() + QStringLiteral("/.config/audioscout");
    QDir().mkpath(d);
    return d;
}

QString Settings::dbPath() const { return configDir() + QStringLiteral("/app.db"); }

QString Settings::previewCacheDir() const {
    QString d = configDir() + QStringLiteral("/previews");
    QDir().mkpath(d);
    return d;
}

QString Settings::undoDir() const {
    QString d = value(QStringLiteral("undo/dir")).toString();
    if (d.isEmpty()) d = configDir() + QStringLiteral("/undo");
    QDir().mkpath(d);
    return d;
}

double Settings::volume() const { return value(QStringLiteral("volume"), 0.8).toDouble(); }
void Settings::setVolume(double v) { setValue(QStringLiteral("volume"), v); emit changed(); }

QString Settings::tablePreviewMode() const { return value(QStringLiteral("tablePreviewMode"), QStringLiteral("wave")).toString(); }
void Settings::setTablePreviewMode(const QString &m) { setValue(QStringLiteral("tablePreviewMode"), m); emit changed(); }

QString Settings::playerMode() const { return value(QStringLiteral("playerMode"), QStringLiteral("wave")).toString(); }
void Settings::setPlayerMode(const QString &m) { setValue(QStringLiteral("playerMode"), m); emit changed(); }

bool Settings::spaceStartStop() const { return value(QStringLiteral("spaceBehavior"), true).toBool(); }
void Settings::setSpaceStartStop(bool v) { setValue(QStringLiteral("spaceBehavior"), v); emit changed(); }

int Settings::undoDepth() const { return value(QStringLiteral("undo/depth"), 20).toInt(); }
void Settings::setUndoDepth(int v) { setValue(QStringLiteral("undo/depth"), v); emit changed(); }

qint64 Settings::undoDiskLimitBytes() const {
    return value(QStringLiteral("undo/diskLimitMB"), qint64(2048)).toLongLong() * 1024LL * 1024LL;
}
void Settings::setUndoDiskLimit(qint64 bytes) {
    setValue(QStringLiteral("undo/diskLimitMB"), bytes / (1024 * 1024));
    emit changed();
}
void Settings::setUndoDir(const QString &d) { setValue(QStringLiteral("undo/dir"), d); emit changed(); }

int Settings::previewPrefetchCount() const { return value(QStringLiteral("previewPrefetch"), 30).toInt(); }
void Settings::setPreviewPrefetchCount(int n) { setValue(QStringLiteral("previewPrefetch"), n); emit changed(); }

QStringList Settings::visibleColumns() const {
    QStringList def = {"name","preview","duration","samplerate","sampletype",
                       "channels","size","comment","created","modified"};
    return value(QStringLiteral("columns/order"), def).toStringList();
}
void Settings::setVisibleColumns(const QStringList &cols) {
    setValue(QStringLiteral("columns/order"), cols);
    emit changed();
}

QString Settings::defaultShortcut(const QString &id) const { return kDefaultHotkeys.value(id); }

QString Settings::shortcut(const QString &actionId) const {
    QVariant v = value(QStringLiteral("hotkeys/") + actionId);
    if (v.isValid() && !v.toString().isEmpty()) return v.toString();
    return defaultShortcut(actionId);
}

void Settings::setShortcut(const QString &actionId, const QString &seq) {
    if (seq == defaultShortcut(actionId))
        removeKey(QStringLiteral("hotkeys/") + actionId);
    else
        setValue(QStringLiteral("hotkeys/") + actionId, seq);
    emit changed();
}

void Settings::resetShortcut(const QString &actionId) {
    removeKey(QStringLiteral("hotkeys/") + actionId);
    emit changed();
}

QStringList Settings::hotkeyActions() const { return QStringList(kDefaultHotkeys.keys()); }
