#include "AudioView.h"
#include "Renderer.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QDateTime>
#include <algorithm>
#include <cmath>

static const int kMarkerHitPx = 6;

AudioView::AudioView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void AudioView::setData(const AudioData *audio) {
    audio_ = audio;
    viewStart_ = 0; viewLen_ = 0; selStart_ = selEnd_ = -1; cursorFrame_ = 0;
    activeMarker_ = -1;
    rebuildBaseImage();
    update();
}

void AudioView::setMode(const QString &mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    rebuildBaseImage();
    update();
}

void AudioView::setMarkers(const QVector<Marker> *m) { markers_ = m; update(); }

void AudioView::setPlayheadFrame(qint64 f) {
    cursorFrame_ = f;
    // auto-scroll during playback
    if (viewLen_ > 0 && audio_) {
        if (f < viewStart_ || f > viewStart_ + viewLen_)
            scrollToFrame(f - viewLen_ / 2);
    }
    update();
}

void AudioView::setActiveMarkerIndex(int idx) {
    activeMarker_ = idx;
    update();
}

void AudioView::setSelectionRange(qint64 a, qint64 b) {
    selStart_ = std::min(a, b); selEnd_ = std::max(a, b);
    emit selectionChanged(selStart_, selEnd_);
    update();
}

void AudioView::clearSelection() {
    selStart_ = selEnd_ = -1;
    emit selectionChanged(-1, -1);
    update();
}

qint64 AudioView::xToFrame(int x) const {
    if (!audio_) return 0;
    qint64 total = audio_->totalFrames;
    qint64 len = viewLen_ > 0 ? viewLen_ : total;
    qint64 start = viewLen_ > 0 ? viewStart_ : 0;
    double frac = double(x) / std::max(1, width());
    return std::clamp<qint64>(start + qint64(frac * len), 0, total);
}

int AudioView::frameToX(qint64 f) const {
    qint64 total = audio_ ? audio_->totalFrames : 1;
    qint64 len = viewLen_ > 0 ? viewLen_ : total;
    qint64 start = viewLen_ > 0 ? viewStart_ : 0;
    if (len <= 0) return 0;
    return int(double(f - start) / len * width());
}

void AudioView::scrollToFrame(qint64 f) {
    if (!audio_) return;
    qint64 len = viewLen_ > 0 ? viewLen_ : audio_->totalFrames;
    viewStart_ = std::clamp<qint64>(f, 0, std::max<qint64>(0, audio_->totalFrames - len));
    if (viewLen_ == 0) viewStart_ = 0;
    update();
}

void AudioView::applyZoom(double factor, int mouseX) {
    if (!audio_) return;
    qint64 total = audio_->totalFrames;
    qint64 curLen = viewLen_ > 0 ? viewLen_ : total;
    qint64 centerFrame = xToFrame(mouseX);
    qint64 newLen = std::clamp<qint64>(qint64(curLen / factor), 16, total);
    double ratio = double(centerFrame - (viewLen_ > 0 ? viewStart_ : 0)) / std::max<qint64>(1, curLen);
    viewLen_ = (newLen >= total) ? 0 : newLen;
    if (viewLen_ > 0) {
        viewStart_ = std::clamp<qint64>(centerFrame - qint64(ratio * newLen), 0, total - newLen);
    } else {
        viewStart_ = 0;
    }
    sampleLevel_ = viewLen_ > 0 && (double(total) / viewLen_) > 40.0 &&
                  viewLen_ < 8192; // deep zoom: show individual samples
    rebuildBaseImage();
    update();
}

void AudioView::zoomIn()  { applyZoom(1.5, width() / 2); }
void AudioView::zoomOut() { applyZoom(1.0 / 1.5, width() / 2); }

void AudioView::zoomReset() {
    viewStart_ = 0; viewLen_ = 0; sampleLevel_ = false;
    rebuildBaseImage();
    update();
}

void AudioView::rebuildBaseImage() {
    base_ = QImage();
    if (!audio_ || !audio_->valid) return;
    // Render at widget resolution for the current view window.
    int w = std::max(64, width());
    int h = std::max(64, height());
    if (sampleLevel_ && mode_ == QLatin1String("wave")) {
        base_ = QImage(w, h, QImage::Format_ARGB32);
        base_.fill(QColor(24, 24, 28));
        QPainter p(&base_);
        p.setRenderHint(QPainter::Antialiasing);
        // draw individual samples as dots/bars
        const int ch = audio_->channels;
        qint64 len = viewLen_ > 0 ? viewLen_ : audio_->totalFrames;
        qint64 start = viewLen_ > 0 ? viewStart_ : 0;
        p.setPen(QColor(90, 200, 120));
        for (int x = 0; x < w; ++x) {
            qint64 f = start + qint64(double(x) / w * len);
            if (f >= audio_->totalFrames) break;
            float mono = 0.f;
            for (int c = 0; c < ch; ++c) mono += audio_->samples[f * ch + c];
            int y = h / 2 - int(mono * (h / 2 - 4));
            p.drawLine(x, h / 2, x, y);
            p.drawPoint(x, y);
        }
        return;
    }
    if (mode_ == QLatin1String("spectrogram"))
        base_ = Renderer::spectrogram(*audio_, w, h);
    else
        base_ = Renderer::waveform(*audio_, w, h);

    if (viewLen_ > 0 && audio_->totalFrames > 0) {
        // crop the full-file image to the visible range
        double scale = double(base_.width()) / audio_->totalFrames;
        int sx = int(viewStart_ * scale);
        int sw = std::max(1, int(viewLen_ * scale));
        sw = std::min(sw, base_.width() - sx);
        base_ = base_.copy(sx, 0, sw, base_.height());
    }
}

void AudioView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 28));
    if (!audio_ || !audio_->valid) {
        p.setPen(Qt::gray);
        p.drawText(rect(), Qt::AlignCenter, tr("No audio loaded"));
        return;
    }
    if (base_.isNull()) rebuildBaseImage();
    p.drawImage(rect(), base_);

    // selection overlay
    if (hasSelection()) {
        int x1 = frameToX(selStart_), x2 = frameToX(selEnd_);
        QRect r(x1, 0, x2 - x1, height());
        p.fillRect(r, QColor(80, 140, 255, 70));
        p.setPen(QColor(80, 140, 255));
        p.drawLine(x1, 0, x1, height());
        p.drawLine(x2, 0, x2, height());
        // edge handles
        p.setBrush(QColor(80, 140, 255));
        p.drawRect(x1 - 3, height() / 2 - 8, 6, 16);
        p.drawRect(x2 - 3, height() / 2 - 8, 6, 16);
    }

    // markers: label on top + dashed vertical line
    if (markers_) {
        for (int i = 0; i < markers_->size(); ++i) {
            const Marker &m = (*markers_)[i];
            int x = frameToX(m.timeSamples);
            if (x < -40 || x > width() + 40) continue;
            bool active = (i == activeMarker_);
            QColor col = active ? QColor(255, 200, 60) : QColor(120, 180, 255);
            QPen pen(col, 1, Qt::DashLine);
            p.setPen(pen);
            p.drawLine(x, 18, x, height());
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            QString label = m.name.isEmpty() ? QStringLiteral("M%1").arg(i + 1) : m.name;
            QFontMetrics fm(font());
            int tw = fm.horizontalAdvance(label) + 8;
            p.drawRect(x - 2, 2, tw, 15);
            p.setPen(active ? Qt::black : QColor(20, 20, 30));
            p.drawText(x + 2, 13, label);
        }
    }

    // playhead
    int px = frameToX(cursorFrame_);
    p.setPen(QPen(QColor(255, 90, 90), 2));
    p.drawLine(px, 0, px, height());
    p.setBrush(QColor(255, 90, 90));
    p.setPen(Qt::NoPen);
    QPolygon tri;
    tri << QPoint(px - 6, 0) << QPoint(px + 6, 0) << QPoint(px, 9);
    p.drawPolygon(tri);
}

static int markerIndexAt(const QVector<Marker> *markers, AudioView *v, int x) {
    if (!markers) return -1;
    int best = -1, bestd = kMarkerHitPx + 1;
    for (int i = 0; i < markers->size(); ++i) {
        int mx = v->frameToX((*markers)[i].timeSamples);
        int d = std::abs(mx - x);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

void AudioView::mousePressEvent(QMouseEvent *e) {
    if (!audio_) return;
    int x = e->position().x();
    // selection edges first
    if (hasSelection()) {
        int x1 = frameToX(selStart_), x2 = frameToX(selEnd_);
        if (std::abs(x - x1) <= 5) { drag_ = MoveSelStart; return; }
        if (std::abs(x - x2) <= 5) { drag_ = MoveSelEnd; return; }
    }
    int mi = markerIndexAt(markers_, this, x);
    if (mi >= 0) {
        activeMarker_ = mi;
        emit markerClicked(mi);
        drag_ = MoveMarker;
        dragMarkerIdx_ = mi;
        update();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        drag_ = NewSelection;
        dragStartX_ = x;
        qint64 f = xToFrame(x);
        // click without drag places cursor and clears selection (resolved on release)
        selStart_ = f; selEnd_ = f;
        cursorFrame_ = f;
        emit cursorMoved(f);
        update();
    } else if (e->button() == Qt::MiddleButton) {
        drag_ = Pan;
        dragStartX_ = x;
    }
}

void AudioView::mouseMoveEvent(QMouseEvent *e) {
    if (!audio_) return;
    int x = e->position().x();
    switch (drag_) {
    case NewSelection: {
        qint64 a = xToFrame(dragStartX_), b = xToFrame(x);
        selStart_ = std::min(a, b); selEnd_ = std::max(a, b);
        update();
        break;
    }
    case MoveSelStart:
        selStart_ = std::min(xToFrame(x), selEnd_ - 1);
        update();
        break;
    case MoveSelEnd:
        selEnd_ = std::max(xToFrame(x), selStart_ + 1);
        update();
        break;
    case MoveMarker: {
        if (markers_ && dragMarkerIdx_ >= 0 && dragMarkerIdx_ < markers_->size()) {
            qint64 f = xToFrame(x);
            Marker &m = const_cast<QVector<Marker> *>(markers_)->operator[](dragMarkerIdx_);
            m.timeSamples = f;
            if (audio_->sampleRate)
                m.timeSec = double(f) / audio_->sampleRate;
            emit markerMoved(dragMarkerIdx_, f);
            update();
        }
        break;
    }
    case Pan: {
        qint64 len = viewLen_ > 0 ? viewLen_ : audio_->totalFrames;
        qint64 df = qint64(double(dragStartX_ - x) / width() * len);
        scrollToFrame((viewLen_ > 0 ? viewStart_ : 0) + df);
        dragStartX_ = x;
        break;
    }
    default: break;
    }
}

void AudioView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (!audio_) return;
    int x = e->position().x();
    qint64 f = xToFrame(x);
    bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    int mi = markerIndexAt(markers_, this, x);
    if (mi >= 0) {
        emit markerClicked(mi);   // main window opens editor dialog
        return;
    }
    if (ctrl) {
        emit markerDoubleClickedEmpty(f, true);
        return;
    }
    // select segment between surrounding markers
    if (markers_ && markers_->size() >= 2) {
        QList<qint64> ts;
        for (const auto &m : *markers_) ts << m.timeSamples;
        std::sort(ts.begin(), ts.end());
        qint64 a = 0, b = audio_->totalFrames;
        for (qint64 t : ts) { if (t <= f) a = t; }
        for (qint64 t : ts) { if (t >= f) { b = t; break; } }
        if (a < b) {
            emit doubleClickBetweenMarkers(a, b);
            return;
        }
    }
    // plain double click: reselect all? place cursor
    cursorFrame_ = f;
    emit cursorMoved(f);
    update();
}

void AudioView::wheelEvent(QWheelEvent *e) {
    if (!audio_) return;
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
        double factor = e->angleDelta().y() > 0 ? 1.25 : 0.8;
        applyZoom(factor, int(e->position().x()));
    } else if (e->modifiers().testFlag(Qt::ShiftModifier)) {
        // horizontal scroll
        qint64 step = (viewLen_ > 0 ? viewLen_ : audio_->totalFrames) / 10;
        scrollToFrame(viewStart_ + (e->angleDelta().y() > 0 ? -step : step));
    } else {
        qint64 step = (viewLen_ > 0 ? viewLen_ : audio_->totalFrames) / 10;
        scrollToFrame(viewStart_ + (e->angleDelta().y() > 0 ? -step : step));
    }
    e->accept();
}

void AudioView::keyPressEvent(QKeyEvent *e) {
    if (!audio_) return;
    qint64 step = 1;
    if (viewLen_ > 0) step = std::max<qint64>(1, viewLen_ / 100);
    switch (e->key()) {
    case Qt::Key_Left:  nudgeCursor(-step); return;
    case Qt::Key_Right: nudgeCursor(step); return;
    case Qt::Key_Escape: clearSelection(); return;
    default: break;
    }
    QWidget::keyPressEvent(e);
}

void AudioView::nudgeCursor(int deltaFrames) {
    if (!audio_) return;
    cursorFrame_ = std::clamp<qint64>(cursorFrame_ + deltaFrames, 0, audio_->totalFrames);
    emit cursorMoved(cursorFrame_);
    update();
}
