#include "AudioFile.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocale>
#include <cmath>
#include <sndfile.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <cmath>

// Map a libsndfile sample subtype back to its SF_FORMAT_* constant so save()
// can write the file in the SAME PCM/float subformat as the source instead of
// silently converting everything to Float32.
static int sfSubformatFromSampleFormat(SampleFormat f) {
    switch (f) {
        case SampleFormat::UInt8:   return SF_FORMAT_PCM_S8;
        case SampleFormat::Int16:   return SF_FORMAT_PCM_16;
        case SampleFormat::Int24:   return SF_FORMAT_PCM_24;
        case SampleFormat::Int32:   return SF_FORMAT_PCM_32;
        case SampleFormat::Float32: return SF_FORMAT_FLOAT;
        case SampleFormat::Float64: return SF_FORMAT_DOUBLE;
        default:                    return 0;   // unknown/compressed -> caller picks a default
    }
}

static SampleFormat sfToFormat(int format) {
    int sub = format & SF_FORMAT_SUBMASK;
    switch (sub) {
        case SF_FORMAT_PCM_S8: return SampleFormat::UInt8; // displayed as 8-bit
        case SF_FORMAT_PCM_16: return SampleFormat::Int16;
        case SF_FORMAT_PCM_24: return SampleFormat::Int24;
        case SF_FORMAT_PCM_32: return SampleFormat::Int32;
        case SF_FORMAT_FLOAT:  return SampleFormat::Float32;
        case SF_FORMAT_DOUBLE: return SampleFormat::Float64;
        case SF_FORMAT_ALAW:   return SampleFormat::Alaw;
        case SF_FORMAT_ULAW:   return SampleFormat::Ulaw;
        default:               return SampleFormat::Unknown;
    }
}

// Probe audio parameters (sample rate, channels, frames, sample type, comment).
// Tries libsndfile, then TagLib, then ffprobe as a last resort so that
// compressed formats (mp3/ogg/m4a) still show full metadata in the table.
static bool probeWithFfprobe(const QString &path, AudioData &meta);

bool AudioFile::probe(const QString &path, AudioData &meta, QString *err) {
    SF_INFO info{};
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (sf) {
        meta.sampleRate = info.samplerate;
        meta.channels = info.channels;
        meta.totalFrames = info.frames;
        meta.format = sfToFormat(info.format);
        meta.valid = info.frames > 0 && info.channels > 0;
        // WAV comment via libsndfile strings (ICMT chunk).
        const char *cmt = sf_get_string(sf, SF_STR_COMMENT);
        if (cmt) meta.comment = QString::fromUtf8(cmt);
        sf_close(sf);
        if (meta.valid) return true;
    }
    // TagLib fallback for metadata of compressed formats.
    TagLib::FileRef f(path.toLocal8Bit().constData());
    if (!f.isNull() && f.file() && f.audioProperties()) {
        auto *ap = f.audioProperties();
        meta.sampleRate = ap->sampleRate();
        meta.channels = ap->channels();
        // Use milliseconds to avoid the deprecated length() and keep sub-second precision.
        meta.totalFrames = qint64(ap->lengthInMilliseconds()) * ap->sampleRate() / 1000;
        meta.format = SampleFormat::Unknown;
        meta.valid = true;
        TagLib::PropertyMap props = f.file()->properties();
        if (props.contains(TagLib::String("COMMENT")))
            meta.comment = QString::fromUtf8(props[TagLib::String("COMMENT")][0].toCString(true));
        else if (props.contains(TagLib::String("DESCRIPTION")))
            meta.comment = QString::fromUtf8(props[TagLib::String("DESCRIPTION")][0].toCString(true));
        else if (f.tag() && !f.tag()->comment().isEmpty())
            meta.comment = QString::fromUtf8(f.tag()->comment().toCString(true));
        return true;
    }
    // Last resort: ffprobe (handles mp3/ogg/m4a when TagLib fails).
    if (probeWithFfprobe(path, meta))
        return true;
    if (err) *err = QStringLiteral("Unsupported or corrupted file");
    return false;
}

static bool probeWithFfprobe(const QString &path, AudioData &meta) {
    QProcess p;
    p.setProgram(QStringLiteral("ffprobe"));
    p.setArguments({"-v", "quiet", "-print_format", "json",
                    "-show_streams", "-show_format", path});
    p.start();
    // Note: -1 (block until finished) must NOT be passed to waitFor* directly,
    // since the int parameter would be truncated to 0 ms and the process would
    // always "time out" instantly. Use a large explicit timeout instead.
    if (!p.waitForStarted(5000)) {
        qWarning() << "probeWithFfprobe: failed to start ffprobe for" << path;
        return false;
    }
    if (!p.waitForFinished(30000)) {
        qWarning() << "probeWithFfprobe: ffprobe timed out for" << path;
        p.kill();
        p.waitForFinished(2000);
        return false;
    }
    if (p.exitCode() != 0) {
        qWarning() << "probeWithFfprobe: ffprobe exit" << p.exitCode()
                   << "stderr:" << QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    for (const QJsonValue &sv : root.value(QStringLiteral("streams")).toArray()) {
        const QJsonObject s = sv.toObject();
        if (s.value(QStringLiteral("codec_type")).toString() != QLatin1String("audio"))
            continue;
        meta.sampleRate = s.value(QStringLiteral("sample_rate")).toInt();
        meta.channels = s.value(QStringLiteral("channels")).toInt();
        double durSec = s.value(QStringLiteral("duration")).toDouble(
                        root.value(QStringLiteral("format")).toObject()
                            .value(QStringLiteral("duration")).toDouble());
        meta.totalFrames = qint64(std::llround(durSec * meta.sampleRate));
        const QString pf = s.value(QStringLiteral("pcm_format")).toString();
        const int bits = s.value(QStringLiteral("bits_per_raw_sample")).toInt(0);
        if (!pf.isEmpty() || bits > 0) {   // uncompressed stream
            if (pf == QLatin1String("f32le") || pf == QLatin1String("f32be"))
                meta.format = SampleFormat::Float32;
            else if (pf == QLatin1String("f64le"))
                meta.format = SampleFormat::Float64;
            else if (bits >= 24) meta.format = SampleFormat::Int24;
            else if (bits >= 16) meta.format = SampleFormat::Int16;
            else if (bits > 0)   meta.format = SampleFormat::UInt8;
        } else {
            meta.format = SampleFormat::Unknown;   // compressed
        }
        meta.valid = meta.sampleRate > 0 && meta.channels > 0 && meta.totalFrames > 0;
        // Comment from tags if present.
        const QJsonObject tags = s.value(QStringLiteral("tags")).toObject().isEmpty()
                                 ? root.value(QStringLiteral("format")).toObject()
                                       .value(QStringLiteral("tags")).toObject()
                                 : s.value(QStringLiteral("tags")).toObject();
        for (const QString &key : {QStringLiteral("comment"), QStringLiteral("COMMENT"),
                                   QStringLiteral("description")}) {
            if (tags.contains(key)) {
                meta.comment = tags.value(key).toString();
                break;
            }
        }
        return meta.valid;
    }
    return false;
}

bool AudioFile::decodeWithFfmpeg(const QString &path, QString &tempOut, QString *err) {
    static QTemporaryDir tmpDir;   // lives for the whole app session
    if (!tmpDir.isValid()) {
        if (err) *err = QStringLiteral("Cannot create temp dir");
        return false;
    }
    const QFileInfo srcFi(path);
    // Cache key includes mtime+size so a changed source file is re-decoded.
    QString outPath = tmpDir.filePath(QStringLiteral("dec_%1_%2_%3.wav")
                    .arg(qHash(srcFi.absoluteFilePath()), 8, 16, QLatin1Char('0'))
                    .arg(srcFi.lastModified().toMSecsSinceEpoch())
                    .arg(srcFi.size()));
    if (QFileInfo::exists(outPath)) { tempOut = outPath; return true; } // cached decode

    QProcess p;
    p.setProgram(QStringLiteral("ffmpeg"));
    // Keep the ORIGINAL sample rate: forcing -ar 44100 would silently resample
    // material recorded at other rates and desync markers/playhead positions.
    p.setArguments({"-y", "-loglevel", "error", "-i", path,
                    "-acodec", "pcm_f32le", outPath});
    p.start();
    if (!p.waitForStarted(5000)) {
        if (err) *err = QStringLiteral("Failed to start ffmpeg (is it installed?)");
        return false;
    }
    if (!p.waitForFinished(120000) || p.exitCode() != 0) {
        QFile::remove(outPath);
        if (err) *err = QStringLiteral("ffmpeg decode failed: ") +
                       QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    tempOut = outPath;
    return true;
}

static bool loadViaSndfile(const QString &path, AudioData &out, QString *err) {
    SF_INFO info{};
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (!sf) { if (err) *err = QString::fromUtf8(sf_strerror(nullptr)); return false; }
    out.sampleRate = info.samplerate;
    out.channels = info.channels;
    out.totalFrames = info.frames;
    out.format = sfToFormat(info.format);
    out.container = info.format & SF_FORMAT_CONTAINERMASK;
    const char *cmt = sf_get_string(sf, SF_STR_COMMENT);
    if (cmt) out.comment = QString::fromUtf8(cmt);
    out.samples.resize(out.totalFrames * out.channels);
    sf_count_t got = sf_readf_float(sf, out.samples.data(), out.totalFrames);
    sf_close(sf);
    if (got <= 0) { if (err) *err = QStringLiteral("No audio data"); return false; }
    out.valid = true;
    return true;
}

bool AudioFile::load(const QString &path, AudioData &out, QString *err) {
    if (loadViaSndfile(path, out, err))
        return true;
    // Fallback: decode with ffmpeg to a temporary WAV.
    QString tmp;
    if (decodeWithFfmpeg(path, tmp, err)) {
        QString e2;
        if (loadViaSndfile(tmp, out, &e2))
            return true;
    }
    out = AudioData{};
    return false;
}

bool AudioFile::save(const QString &path, const AudioData &data, QString *err) {
    QFileInfo fi(path);
    if (fi.exists() && !fi.isWritable()) {
        if (err) *err = QStringLiteral("No write permission for %1").arg(path);
        return false;
    }
    QString lower = path.toLower();
    int container = SF_FORMAT_WAV;
    if (lower.endsWith(QLatin1String(".aif")) || lower.endsWith(QLatin1String(".aiff")))
        container = SF_FORMAT_AIFF;
    else if (lower.endsWith(QLatin1String(".caf")))
        container = SF_FORMAT_CAF;
    // Preserve the source subformat (e.g. PCM-16 stays PCM-16) so editing a
    // file does not silently convert it to Float32. Fall back to Float32 for
    // unknown/compressed sources that were decoded via ffmpeg.
    int sub = sfSubformatFromSampleFormat(data.format);
    if (sub == 0) sub = SF_FORMAT_FLOAT;
    int format = container | sub;

    SF_INFO info{};
    info.samplerate = data.sampleRate;
    info.channels = data.channels;
    info.format = format;
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SFM_WRITE, &info);
    if (!sf) { if (err) *err = QString::fromUtf8(sf_strerror(nullptr)); return false; }
    // Header strings must be set BEFORE writing samples: after the PCM data is
    // committed, libsndfile can no longer insert the ICMT/text chunk.
    if (!data.comment.isEmpty())
        sf_set_string(sf, SF_STR_COMMENT, data.comment.toUtf8().constData());
    sf_count_t wrote = sf_writef_float(sf, data.samples.data(), data.totalFrames);
    sf_close(sf);
    if (wrote != data.totalFrames) {
        if (err) *err = QStringLiteral("Incomplete write");
        return false;
    }
    return true;
}
