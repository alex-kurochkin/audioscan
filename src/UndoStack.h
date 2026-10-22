#pragma once
#include <QVector>
#include <QDateTime>
#include <QString>
#include "AudioFile.h"
#include "Types.h"

// Undo/redo stack for editing operations. Original samples of modified ranges
// are written to files in the undo directory; only metadata stays in memory.
struct UndoRecord {
    QString type;              // "mute", "normalize", "delete"
    QString filePath;
    qint64 offset = 0;         // frame offset of affected range
    qint64 length = 0;         // frames in affected range (before op)
    qint64 newLength = 0;      // frames after op (for delete: length - removed)
    double normalizeDbfs = 0;  // target level captured at operation time (redo uses this, not the current spinbox)
    QString samplesFile;       // path to raw float32 original data (interleaved)
    int channels = 0;
    // marker bookkeeping for delete ops
    struct MarkerDelta {
        Marker m;
        bool deleted;          // was inside removed region
        qint64 oldPos, newPos;
    };
    QVector<MarkerDelta> markerDeltas;
    QDateTime timestamp;
};

class UndoStack {
public:
    // Push a record whose original samples were already saved via saveSamples().
    void push(const UndoRecord &rec);
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }
    const UndoRecord *takeUndo();
    const UndoRecord *takeRedo();

    static QString writeOriginalSamples(const QString &undoDir, const AudioData &data,
                                        qint64 startFrame, qint64 endFrame, QString *err);
    static bool readOriginalSamples(const QString &file, std::vector<float> &out, QString *err);

    void clear();
    // Enforce depth and disk limits; oldest records dropped first.
    void enforceLimits(int maxDepth, qint64 maxBytes);

private:
    QVector<UndoRecord> undo_, redo_;
};
