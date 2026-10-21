#pragma once
#include <QObject>
#include <QImage>
#include <QString>

// Background preview generator: decodes audio (libsndfile/ffmpeg), renders
// 340x20 wave/spectrogram, stores PNG in the preview cache dir under a UUIDv4
// name and records the mapping in SQLite. Emits ready() on the main thread.
class PreviewWorker : public QObject {
    Q_OBJECT
public:
    explicit PreviewWorker(QObject *parent = nullptr);
    void request(const QString &path, const QString &mode);
    bool hasPending(const QString &path) const;

signals:
    void ready(const QString &path, const QString &imagePath);

private:
    class Runner;
    Runner *runner_;
};
