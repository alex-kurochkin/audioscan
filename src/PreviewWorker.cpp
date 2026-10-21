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
    // Graceful shutdown: wakes the waiting loop so it can exit its thread
    // instead of being terminated mid-database-write at app exit.
    ~Runner() override {
        {
            QMutexLocker l(&mtx);
            quit_ = true;
        }
        cond.wakeAll();
        if (thread_) thread_->wait(3000);
    }
    void setThread(QThread *t) { thread_ = t; }
    void enqueue(const QString &path, const QString &mode) {
        QMutexLocker l(&mtx);
        if (quit_) return;
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

public slots:
    void loop() {
        while (true) {
            Job job;
            {
                QMutexLocker l(&mtx);
                while (queue.isEmpty() && !quit_) cond.wait(&mtx);
                if (quit_) return;          // drain-exit: never terminate mid-render
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
    bool quit_ = false;
    QThread *thread_ = nullptr;

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
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("PreviewWorker"));
    runner_->moveToThread(thread_);
    runner_->setThread(thread_);
    connect(thread_, &QThread::started, runner_, &Runner::loop);
    connect(runner_, &Runner::done, this, [this](const QString &p, const QString &i) {
        emit ready(p, i);
    });
    thread_->start();
}

PreviewWorker::~PreviewWorker() {
    // Ask the loop to exit and stop the thread cleanly: quitting here used to
    // leave a running thread that Qt terminated at app exit — sometimes in the
    // middle of a SQLite write, corrupting the preview cache.
    if (thread_ && thread_->isRunning()) {
        thread_->requestInterruption();
        thread_->quit();
        if (!thread_->wait(3000))
            thread_->terminate();
    }
    delete runner_;   // Runner dtor also signals quit_ and waits for the thread
    runner_ = nullptr;
}

void PreviewWorker::request(const QString &path, const QString &mode) {
    runner_->enqueue(path, mode);
}

bool PreviewWorker::hasPending(const QString &path) const {
    return runner_->pending(path);
}
