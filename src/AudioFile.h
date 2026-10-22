#pragma once
#include <QString>
#include <QVector>
#include "Types.h"

// Decoded PCM buffer: interleaved float [-1..1] frames.
struct AudioData {
    int sampleRate = 44100;
    int channels = 2;
    SampleFormat format = SampleFormat::Float32;
    int container = 0x010000;   // libsndfile container (SF_FORMAT_WAV/-AIFF/-CAF) of the source file
    qint64 totalFrames = 0;
    std::vector<float> samples;      // interleaved
    QString comment;             // ICMT / Vorbis comment / TagLib comment
    bool valid = false;

    double durationSec() const { return sampleRate ? double(totalFrames) / sampleRate : 0.0; }
};

class AudioFile {
public:
    // Try libsndfile first (WAV/AIFF/CAF/FLAC). On failure, decode via ffmpeg to temp WAV.
    static bool load(const QString &path, AudioData &out, QString *err = nullptr);

    // Read-only metadata probe without full decode (for table scanning). Returns false if broken.
    static bool probe(const QString &path, AudioData &meta, QString *err = nullptr);

    // Save PCM data back to an editable file (WAV/AIFF/CAF), preserving format params.
    static bool save(const QString &path, const AudioData &data, QString *err = nullptr);

    // Decode via ffmpeg (QProcess) into a temporary WAV path returned in tempOut.
    static bool decodeWithFfmpeg(const QString &path, QString &tempOut, QString *err = nullptr);
};
