#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

// Audio sample format description (mapped from libsndfile).
enum class SampleFormat {
    Unknown,
    Int16,
    Int24,          // stored as int32 frames
    Int32,
    Float32,
    Float64,
    UInt8,
    Alaw,
    Ulaw
};

inline QString sampleFormatName(SampleFormat f) {
    switch (f) {
        case SampleFormat::Int16:   return QStringLiteral("Int16");
        case SampleFormat::Int24:   return QStringLiteral("Int24");
        case SampleFormat::Int32:   return QStringLiteral("Int32");
        case SampleFormat::Float32: return QStringLiteral("Float32");
        case SampleFormat::Float64: return QStringLiteral("Float64");
        case SampleFormat::UInt8:   return QStringLiteral("UInt8");
        case SampleFormat::Alaw:    return QStringLiteral("ALaw");
        case SampleFormat::Ulaw:    return QStringLiteral("ULaw");
        default:                    return QStringLiteral("?");
    }
}

struct Marker {
    qint64 id = -1;            // DB id, -1 if not yet persisted
    QString name;
    qint64 timeSamples = 0;
    double timeSec = 0.0;
};

// Editable formats: WAV/AIFF/CAF PCM. Read-only playback: flac/mp3/ogg/m4a via ffmpeg fallback.
inline bool isEditableFormat(const QString &pathLower) {
    return pathLower.endsWith(QLatin1String(".wav"))
        || pathLower.endsWith(QLatin1String(".aiff"))
        || pathLower.endsWith(QLatin1String(".aif"))
        || pathLower.endsWith(QLatin1String(".caf"));
}

inline bool isAudioFile(const QString &pathLower) {
    // Container/codec extensions that actually carry audio streams playable by
    // the app (libsndfile native + ffmpeg-decodable). ".mp4" alone is usually a
    // video container and ".aiffc" is not a real extension — both removed.
    static const char *exts[] = {".wav",".aiff",".aif",".caf",".flac",".mp3",".ogg",".oga",
                                 ".m4a",".aac",".wma",".opus",".wv",".au",".mp2"};
    for (const char *e : exts)
        if (pathLower.endsWith(QLatin1String(e))) return true;
    return false;
}
