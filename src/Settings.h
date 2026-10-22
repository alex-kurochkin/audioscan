#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QKeySequence>
#include <QVariantMap>
#include <QJsonObject>

// Static settings stored in ~/.config/<app>/config.json (plain JSON key/value).
class Settings : public QObject {
    Q_OBJECT
public:
    static Settings &instance();

    QString configDir() const;      // ~/.config/audioscout
    QString dbPath() const;         // configDir/app.db
    QString previewCacheDir() const;// configDir/previews
    QString undoDir() const;        // settings "undo/dir", default configDir/undo

    double volume() const;              // 0..1
    void setVolume(double v);

    QString tablePreviewMode() const;   // "wave" | "spectrogram"
    void setTablePreviewMode(const QString &m);
    QString playerMode() const;
    void setPlayerMode(const QString &m);

    bool spaceStartStop() const;        // true: start/stop, false: start/pause
    void setSpaceStartStop(bool v);

    int undoDepth() const;              // default 20
    void setUndoDepth(int v);
    qint64 undoDiskLimitBytes() const;  // default 2 GB
    void setUndoDiskLimit(qint64 bytes);
    void setUndoDir(const QString &d);

    int previewPrefetchCount() const;   // default 30
    void setPreviewPrefetchCount(int n);

    QStringList visibleColumns() const; // ordered column keys (visible ones)
    void setVisibleColumns(const QStringList &cols);
    // Full display order of *all* columns (visible + hidden), so that the
    // user's drag-reorder survives restarts even for currently hidden cols.
    QStringList columnOrder() const;
    void setColumnOrder(const QStringList &cols);

    // Hotkeys: action id -> shortcut string (defaults if unset).
    QString shortcut(const QString &actionId) const;
    void setShortcut(const QString &actionId, const QString &seq);
    void resetShortcut(const QString &actionId);
    QStringList hotkeyActions() const;
    QString defaultShortcut(const QString &actionId) const;

signals:
    void changed();

private:
    Settings();
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &v);
    void removeKey(const QString &key);
    QJsonObject data_;
    QString filePath_;
    void load();
    void save();
};
