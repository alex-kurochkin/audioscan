#include "Settings.h"
#include <QStandardPaths>
#include <QDir>
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

Settings::Settings()
    : s_(configDir() + QStringLiteral("/config.json"), QSettings::JsonFormat) {
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
    QString d = s_.value(QStringLiteral("undo/dir")).toString();
    if (d.isEmpty()) d = configDir() + QStringLiteral("/undo");
    QDir().mkpath(d);
    return d;
}

double Settings::volume() const { return s_.value(QStringLiteral("volume"), 0.8).toDouble(); }
void Settings::setVolume(double v) { s_.setValue(QStringLiteral("volume"), v); emit changed(); }

QString Settings::tablePreviewMode() const { return s_.value(QStringLiteral("tablePreviewMode"), QStringLiteral("wave")).toString(); }
void Settings::setTablePreviewMode(const QString &m) { s_.setValue(QStringLiteral("tablePreviewMode"), m); emit changed(); }

QString Settings::playerMode() const { return s_.value(QStringLiteral("playerMode"), QStringLiteral("wave")).toString(); }
void Settings::setPlayerMode(const QString &m) { s_.setValue(QStringLiteral("playerMode"), m); emit changed(); }

bool Settings::spaceStartStop() const { return s_.value(QStringLiteral("spaceBehavior"), true).toBool(); }
void Settings::setSpaceStartStop(bool v) { s_.setValue(QStringLiteral("spaceBehavior"), v); emit changed(); }

int Settings::undoDepth() const { return s_.value(QStringLiteral("undo/depth"), 20).toInt(); }
void Settings::setUndoDepth(int v) { s_.setValue(QStringLiteral("undo/depth"), v); emit changed(); }

qint64 Settings::undoDiskLimitBytes() const {
    return s_.value(QStringLiteral("undo/diskLimitMB"), qint64(2048)).toLongLong() * 1024LL * 1024LL;
}
void Settings::setUndoDiskLimit(qint64 bytes) {
    s_.setValue(QStringLiteral("undo/diskLimitMB"), bytes / (1024 * 1024));
    emit changed();
}
void Settings::setUndoDir(const QString &d) { s_.setValue(QStringLiteral("undo/dir"), d); emit changed(); }

int Settings::previewPrefetchCount() const { return s_.value(QStringLiteral("previewPrefetch"), 30).toInt(); }
void Settings::setPreviewPrefetchCount(int n) { s_.setValue(QStringLiteral("previewPrefetch"), n); emit changed(); }

QStringList Settings::visibleColumns() const {
    QStringList def = {"name","preview","duration","samplerate","sampletype",
                       "channels","size","comment","created","modified"};
    return s_.value(QStringLiteral("columns/order"), def).toStringList();
}
void Settings::setVisibleColumns(const QStringList &cols) {
    s_.setValue(QStringLiteral("columns/order"), cols);
    emit changed();
}

QString Settings::defaultShortcut(const QString &id) const { return kDefaultHotkeys.value(id); }

QString Settings::shortcut(const QString &actionId) const {
    QVariant v = s_.value(QStringLiteral("hotkeys/") + actionId);
    if (v.isValid() && !v.toString().isEmpty()) return v.toString();
    return defaultShortcut(actionId);
}

void Settings::setShortcut(const QString &actionId, const QString &seq) {
    if (seq == defaultShortcut(actionId))
        s_.remove(QStringLiteral("hotkeys/") + actionId);
    else
        s_.setValue(QStringLiteral("hotkeys/") + actionId, seq);
    emit changed();
}

void Settings::resetShortcut(const QString &actionId) {
    s_.remove(QStringLiteral("hotkeys/") + actionId);
    emit changed();
}

QStringList Settings::hotkeyActions() const { return QStringList(kDefaultHotkeys.keys()); }
