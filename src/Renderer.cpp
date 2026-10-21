#include "Renderer.h"
#include <QPainter>
#include <cmath>
#include <vector>

// ---- Minimal iterative radix-2 FFT ----
static void fftInPlace(std::vector<std::complex<float>> &a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * float(M_PI) / float(len);
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k];
                auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void Renderer::fftMagnitude(const float *window, int n, QVector<float> &magOut) {
    std::vector<std::complex<float>> buf(n);
    for (int i = 0; i < n; ++i) {
        // Hann window
        float w = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (n - 1));
        buf[i] = {window[i] * w, 0.0f};
    }
    fftInPlace(buf);
    magOut.resize(n / 2);
    for (int i = 0; i < n / 2; ++i)
        magOut[i] = std::abs(buf[i]);
}

QImage Renderer::waveform(const AudioData &audio, int width, int height, bool) {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(QColor(30, 30, 34));
    if (!audio.valid || audio.totalFrames == 0) return img;

    QPainter p(&img);
    QColor wave(90, 200, 120);
    const qint64 framesPerCol = std::max<qint64>(1, audio.totalFrames / width);
    const int ch = audio.channels;
    int prevMid = -1, prevLo = -1, prevHi = -1;
    for (int x = 0; x < width; ++x) {
        qint64 start = qint64(x) * framesPerCol;
        qint64 end = std::min(audio.totalFrames, start + framesPerCol);
        if (start >= audio.totalFrames) break;
        float mn = 1e9f, mx = -1e9f;
        for (qint64 f = start; f < end; ++f) {
            float mono = 0.f;
            for (int c = 0; c < ch; ++c) mono += audio.samples[f * ch + c];
            mono /= ch;
            mn = std::min(mn, mono);
            mx = std::max(mx, mono);
        }
        int mid = height / 2;
        int lo = mid + int(mn * (height / 2 - 1));
        int hi = mid + int(mx * (height / 2 - 1));
        if (hi > lo) std::swap(hi, lo);
        for (int y = hi; y <= lo; ++y) p.setPixel(x, y, wave.rgb());
        if (prevLo >= 0 && lo == hi) { // fill gaps between columns
            p.setPen(wave);
            p.drawLine(x - 1, std::clamp(prevMid, 0, height - 1), x, mid);
        }
        prevMid = mid; prevLo = lo; prevHi = hi;
    }
    return img;
}

QImage Renderer::spectrogram(const AudioData &audio, int width, int height) {
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::black);
    if (!audio.valid || audio.totalFrames < 512) return img;

    const int fftSize = 512;
    const qint64 hop = std::max<qint64>(1, audio.totalFrames / width);
    const int ch = audio.channels;
    std::vector<float> win(fftSize);
    QImage scaled = img;
    QPainter p(&scaled);
    QVector<float> mag;
    for (int x = 0; x < width; ++x) {
        qint64 base = qint64(x) * hop;
        if (base + fftSize > audio.totalFrames) break;
        for (int i = 0; i < fftSize; ++i) {
            float mono = 0.f;
            for (int c = 0; c < ch; ++c) mono += audio.samples[(base + i) * ch + c];
            win[i] = mono / ch;
        }
        fftMagnitude(win.data(), fftSize, mag);
        // map log frequency bins to rows (bottom = low freq)
        for (int y = 0; y < height; ++y) {
            double t = 1.0 - double(y) / height;   // 0 bottom..1 top
            int bin = int((std::pow(t, 2.0)) * (mag.size() - 1));
            float m = mag[bin];
            float db = 20.f * std::log10(m + 1e-7f);
            float norm = std::clamp((db + 80.f) / 80.f, 0.f, 1.f);
            // simple heat colormap
            int r = int(norm * 255);
            int g = int(norm * norm * 200);
            int b = int((1 - norm) * 80 + norm * 40);
            p.setPen(QColor(r, g, b));
            p.drawPoint(x, y);
        }
    }
    return scaled;
}
