#include "AudioFile.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <sndfile.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <cmath>

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

bool AudioFile::probe(const QString &path, AudioData &meta, QString *err) {
    SF_INFO info{};
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SF_MODE_READ, &info);
    if (sf) {
        meta.sampleRate = info.samplerate;
        meta.channels = info.channels;
        meta.totalFrames = info.frames;
        meta.format = sfToFormat(info.format);
        meta.valid = info.frames > 0 && info.channels > 0;
        // WAV comment via libsndfile strings (ICMT chunk).
        const char *cmt = sf_get_string(sf, SF_STR_COMMENTS);
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
        meta.totalFrames = qint64(ap->length()) * ap->sampleRate();
        meta.format = SampleFormat::Unknown;
        meta.valid = true;
        PropertyMap props = f.file()->properties();
        if (props.contains(QLatin1String("COMMENT")))
            meta.comment = props[QLatin1String("COMMENT")][0];
        else if (props.contains(QLatin1String("DESCRIPTION")))
            meta.comment = props[QLatin1String("DESCRIPTION")][0];
        else if (f.tag() && !f.tag()->comment().isEmpty())
            meta.comment = QString::fromStdWString(f.tag()->comment().data());
        return true;
    }
    if (err) *err = QStringLiteral("Unsupported or corrupted file");
    return false;
}

bool AudioFile::decodeWithFfmpeg(const QString &path, QString &tempOut, QString *err) {
    static QTemporaryDir tmpDir;   // lives for the whole app session
    if (!tmpDir.isValid()) {
        if (err) *err = QStringLiteral("Cannot create temp dir");
        return false;
    }
    QString outPath = tmpDir.filePath(QStringLiteral("dec_%1.wav")
                    .arg(qHash(QFileInfo(path).absoluteFilePath()), 8, 16, QLatin1Char('0')));
    if (QFileInfo::exists(outPath)) { tempOut = outPath; return true; } // cached decode

    QProcess p;
    p.setProgram(QStringLiteral("ffmpeg"));
    p.setArguments({"-y", "-loglevel", "error", "-i", path,
                    "-acodec", "pcm_f32le", "-ar", "44100", outPath});
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
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SF_MODE_READ, &info);
    if (!sf) { if (err) *err = QString::fromUtf8(sf_strerror(nullptr)); return false; }
    out.sampleRate = info.samplerate;
    out.channels = info.channels;
    out.totalFrames = info.frames;
    out.format = sfToFormat(info.format);
    const char *cmt = sf_get_string(sf, SF_STR_COMMENTS);
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
    int format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    if (lower.endsWith(QLatin1String(".aif")) || lower.endsWith(QLatin1String(".aiff")))
        format = SF_FORMAT_AIFF | SF_FORMAT_FLOAT;
    else if (lower.endsWith(QLatin1String(".caf")))
        format = SF_FORMAT_CAF | SF_FORMAT_FLOAT;

    SF_INFO info{};
    info.samplerate = data.sampleRate;
    info.channels = data.channels;
    info.format = format;
    SNDFILE *sf = sf_open(path.toLocal8Bit().constData(), SF_MODE_WRITE, &info);
    if (!sf) { if (err) *err = QString::fromUtf8(sf_strerror(nullptr)); return false; }
    sf_count_t wrote = sf_writef_float(sf, data.samples.constData(), data.totalFrames);
    if (!data.comment.isEmpty())
        sf_set_string(sf, SF_STR_COMMENTS, data.comment.toUtf8().constData());
    sf_close(sf);
    if (wrote != data.totalFrames) {
        if (err) *err = QStringLiteral("Incomplete write");
        return false;
    }
    return true;
}
