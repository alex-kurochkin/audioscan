#include "PreviewWorker.h"
#include "AudioFile.h"
#include "Renderer.h"
#include "Settings.h"
#include "Database.h"

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QFileInfo>
#include <QUuid>
#include <QDir>
#include <QQueue>
#include <QMetaObject>

class PreviewWorker::Runner : public QObject {
    Q_OBJECT
public:
    void enqueue(const QString &path, const QString &mode) {
        QMutexLocker l(&mtx);
        queue.enqueue({path, mode});
        cond.wakeAll();
    }
    bool pending(const QString &path) {
        QMutexLocker l(&mtx);
        for (const auto &j : queue) if (j.path == path) return true;
        return processing_ == path;
    }
signals:
    void done(const QString &path, const QString &imagePath);

private slots:
    void loop() {
        while (true) {
            Job job;
            {
                QMutexLocker l(&mtx);
                while (queue.isEmpty()) cond.wait(&mtx);
                job = queue.dequeue();
                processing_ = job.path;
            }
            QString imgPath = render(job);
            emit done(job.path, imgPath);
            QMutexLocker l(&mtx);
            processing_.clear();
        }
    }

private:
    struct Job { QString path, mode; };
    QQueue<Job> queue;
    QMutex mtx;
    QWaitCondition cond;
    QString processing_;

    QString render(const Job &job) {
        auto &S = Settings::instance();
        QFileInfo fi(job.path);
        qint64 size = fi.size();
        qint64 mtime = fi.lastModified().toMSecsSinceEpoch();

        // cache hit?
        QString uuid = Database::instance().lookupPreview(job.path, size, mtime, job.mode);
        if (!uuid.isEmpty()) {
            QString existing = S.previewCacheDir() + "/" + uuid + QStringLiteral(".png");
            if (QFileInfo::exists(existing)) return existing;
        }

        AudioData data;
        QString err;
        if (!AudioFile::load(job.path, data, &err))
            return QString();

        QImage img = job.mode == QLatin1String("spectrogram")
            ? Renderer::spectrogram(data, 340, 20)
            : Renderer::waveform(data, 340, 20);

        QString newUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString file = S.previewCacheDir() + "/" + newUuid + QStringLiteral(".png");
        QDir().mkpath(S.previewCacheDir());
        if (!img.save(file, "PNG")) return QString();
        Database::instance().storePreview(job.path, size, mtime, job.mode, newUuid);
        return file;
    }
};

#include "PreviewWorker.moc"

PreviewWorker::PreviewWorker(QObject *parent) : QObject(parent) {
    runner_ = new Runner;
    static QThread thread;
    runner_->moveToThread(&thread);
    connect(&thread, &QThread::started, runner_, &Runner::loop);
    connect(runner_, &Runner::done, this, [this](const QString &p, const QString &i) {
        emit ready(p, i);
    });
    thread.start();
}

void PreviewWorker::request(const QString &path, const QString &mode) {
    runner_->enqueue(path, mode);
}

bool PreviewWorker::hasPending(const QString &path) const {
    return runner_->pending(path);
}
