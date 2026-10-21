#include "UndoStack.h"
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QFileInfo>

void UndoStack::push(const UndoRecord &rec) {
    undo_.append(rec);
    for (const auto &r : redo_) QFile::remove(r.samplesFile);
    redo_.clear();
}

const UndoRecord *UndoStack::takeUndo() {
    if (undo_.isEmpty()) return nullptr;
    UndoRecord rec = undo_.takeLast();
    redo_.append(rec);
    return &redo_.last();
}

const UndoRecord *UndoStack::takeRedo() {
    if (redo_.isEmpty()) return nullptr;
    UndoRecord rec = redo_.takeLast();
    undo_.append(rec);
    return &undo_.last();
}

QString UndoStack::writeOriginalSamples(const QString &undoDir, const AudioData &data,
                                        qint64 startFrame, qint64 endFrame, QString *err) {
    QDir().mkpath(undoDir);
    QString file = undoDir + QStringLiteral("/undo_%1.raw")
                       .arg(QString::fromUtf8(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("Cannot write undo data to %1").arg(undoDir);
        return QString();
    }
    const float *p = data.samples.constData() + startFrame * data.channels;
    qint64 frames = endFrame - startFrame;
    f.write(reinterpret_cast<const char *>(p), frames * data.channels * sizeof(float));
    f.close();
    return file;
}

bool UndoStack::readOriginalSamples(const QString &file, QVector<float> &out, QString *err) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Undo data missing: %1").arg(file);
        return false;
    }
    QByteArray raw = f.readAll();
    out.resize(raw.size() / int(sizeof(float)));
    std::memcpy(out.data(), raw.constData(), raw.size());
    return true;
}

void UndoStack::clear() {
    for (const auto &r : undo_) QFile::remove(r.samplesFile);
    for (const auto &r : redo_) QFile::remove(r.samplesFile);
    undo_.clear();
    redo_.clear();
}

void UndoStack::enforceLimits(int maxDepth, qint64 maxBytes) {
    // depth limit
    while (undo_.size() > maxDepth) {
        QFile::remove(undo_.first().samplesFile);
        undo_.removeFirst();
    }
    // disk limit (sum of stored sample files)
    qint64 total = 0;
    for (const auto &r : undo_) total += QFileInfo(r.samplesFile).size();
    while (!undo_.isEmpty() && total > maxBytes) {
        qint64 sz = QFileInfo(undo_.first().samplesFile).size();
        QFile::remove(undo_.first().samplesFile);
        undo_.removeFirst();
        total -= sz;
    }
}
