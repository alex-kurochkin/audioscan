#pragma once
#include <QImage>
#include "AudioFile.h"

// Rendering helpers for waveform and spectrogram previews / main view.
namespace Renderer {
    // Waveform: min/max envelope per pixel column, mono downmix.
    QImage waveform(const AudioData &audio, int width, int height, bool withGrid = false);

    // Spectrogram: log-frequency mel-ish bands, simple windowed FFT.
    QImage spectrogram(const AudioData &audio, int width, int height);

    // Compute magnitude spectrum of one window (float samples), returns half-spectrum.
    void fftMagnitude(const float *window, int n, QVector<float> &magOut);
}
