#pragma once
#include <QObject>
#include <QImage>
#include <QString>
class QThread;

// Background preview generator: decodes audio (libsndfile/ffmpeg), renders
// 340x20 wave/spectrogram, stores PNG in the preview cache dir under a UUIDv4
// name and records the mapping in SQLite. Emits ready() on the main thread.
class PreviewWorker : public QObject {
    Q_OBJECT
public:
    explicit PreviewWorker(QObject *parent = nullptr);
    ~PreviewWorker() override;
    void request(const QString &path, const QString &mode);
    bool hasPending(const QString &path) const;
    // Drop every queued job (used when the preview mode changes mid-scan so
    // stale renders of the previous mode never reach the table).
    void clearQueue();

signals:
    // path may carry a generation tag suffix ("#gN") supplied by the caller;
    // it is stripped before rendering and echoed back verbatim in ready().
    void ready(const QString &path, const QString &imagePath);

private:
    class Runner;
    Runner *runner_;
    QThread *thread_ = nullptr;   // owned; stopped gracefully in the destructor
};
