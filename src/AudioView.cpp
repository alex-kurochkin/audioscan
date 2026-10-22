#include "AudioView.h"
#include "Renderer.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QDateTime>
#include <QApplication>
#include <algorithm>
#include <cmath>

static const int kMarkerHitPx = 6;

const int AudioView::kScrollBarH = 14;   // reserved strip at the bottom of the widget

namespace {
// Usable drawing height: the bottom strip is reserved for the horizontal
// scrollbar so the waveform/spectrogram never gets covered by it.
inline int viewContentHeight(const QWidget *v) {
    return std::max(32, v->height() - AudioView::kScrollBarH);
}
} // namespace

AudioView::AudioView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Bottom horizontal scrollbar (spec: navigation area must have a scroll
    // bar at the bottom). It mirrors the zoomed view window: page step ==
    // one screenful of frames, and it stays hidden while the whole file fits.
    scrollBar_ = new QScrollBar(Qt::Horizontal, this);
    scrollBar_->setGeometry(0, height() - kScrollBarH, width(), kScrollBarH);
    scrollBar_->hide();
    connect(scrollBar_, &QScrollBar::valueChanged, this, [this](int v) {
        if (scrollSync_) return;               // programmatic update: no re-entry
        if (!audio_) return;
        qint64 len = viewLen_ > 0 ? viewLen_ : audio_->totalFrames;
        viewStart_ = std::clamp<qint64>(qint64(v), 0,
                                        std::max<qint64>(0, audio_->totalFrames - len));
        rebuildBaseImage();
        update();
    });
}

void AudioView::syncScrollBar() {
    if (!scrollBar_) return;
    const bool zoomed = audio_ && audio_->valid && viewLen_ > 0 &&
                        viewLen_ < audio_->totalFrames;
    scrollBar_->setVisible(zoomed);
    scrollBar_->setGeometry(0, std::max(0, height() - kScrollBarH),
                            width(), kScrollBarH);
    if (!zoomed) return;
    const qint64 maxV = std::max<qint64>(0, audio_->totalFrames - viewLen_);
    scrollSync_ = true;
    scrollBar_->setRange(0, int(maxV));
    scrollBar_->setPageStep(int(std::min<qint64>(viewLen_, maxV > 0 ? maxV : 1)));
    scrollBar_->setSingleStep(std::max<qint64>(1, viewLen_ / 50));
    scrollBar_->setValue(int(viewStart_));
    scrollSync_ = false;
}

void AudioView::setData(const AudioData *audio) {
    audio_ = audio;
    viewStart_ = 0; viewLen_ = 0; selStart_ = selEnd_ = -1; cursorFrame_ = 0;
    activeMarker_ = -1;
    sampleLevel_ = false;
    rebuildBaseImage();
    syncScrollBar();
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
    syncScrollBar();
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
        // max() guards against a negative upper bound when the file is shorter
        // than the minimum zoom window (std::clamp aborts if hi < lo).
        viewStart_ = std::clamp<qint64>(centerFrame - qint64(ratio * newLen), 0,
                                        std::max<qint64>(0, total - newLen));
    } else {
        viewStart_ = 0;
    }
    sampleLevel_ = viewLen_ > 0 && (double(total) / viewLen_) > 40.0 &&
                  viewLen_ < 8192; // deep zoom: show individual samples
    rebuildBaseImage();
    syncScrollBar();
    update();
}

void AudioView::zoomIn()  { applyZoom(1.5, width() / 2); }
void AudioView::zoomOut() { applyZoom(1.0 / 1.5, width() / 2); }

void AudioView::zoomReset() {
    viewStart_ = 0; viewLen_ = 0; sampleLevel_ = false;
    rebuildBaseImage();
    syncScrollBar();
    update();
}

void AudioView::rebuildBaseImage() {
    base_ = QImage();
    if (!audio_ || !audio_->valid) return;
    // Render at widget resolution for the current view window. The height
    // excludes the reserved bottom scrollbar strip.
    int w = std::max(64, width());
    int h = std::max(32, viewContentHeight(this));
    if (sampleLevel_) {
        // Deep zoom: draw individual samples instead of min/max envelopes.
        // Works in both display modes — the spectrogram gets a per-sample
        // amplitude plot (its STFT resolution is fixed by the FFT window and
        // cannot resolve single samples, so at this zoom level the raw signal
        // is the only meaningful rendering).
        base_ = QImage(w, h, QImage::Format_ARGB32);
        base_.fill(QColor(24, 24, 28));
        QPainter p(&base_);
        p.setRenderHint(QPainter::Antialiasing);
        const int ch = audio_->channels;
        qint64 len = viewLen_ > 0 ? viewLen_ : audio_->totalFrames;
        qint64 start = viewLen_ > 0 ? viewStart_ : 0;
        const QColor col = (mode_ == QLatin1String("spectrogram"))
                               ? QColor(120, 180, 255)     // spectrogram accent
                               : QColor(90, 200, 120);     // waveform accent
        p.setPen(col);
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
    if (viewLen_ > 0 && audio_->totalFrames > 0) {
        // Zoomed in: render ONLY the visible frame range at full widget
        // resolution. (Rendering the whole file and cropping a few pixels of
        // it produced a blurry, meaningless strip at high zoom levels.)
        const qint64 savedStart = viewStart_, savedLen = viewLen_;
        // The renderers below take an AudioData and ignore the view state, so
        // we can simply hand them a copy trimmed to the visible window.
        const qint64 endFrame = std::min(savedStart + savedLen, audio_->totalFrames);
        AudioData sub = *audio_;
        sub.samples.assign(audio_->samples.begin() + savedStart * audio_->channels,
                           audio_->samples.begin() + endFrame * audio_->channels);
        sub.totalFrames = endFrame - savedStart;
        base_ = (mode_ == QLatin1String("spectrogram"))
                    ? Renderer::spectrogram(sub, w, h)
                    : Renderer::waveform(sub, w, h);
        return;
    }
    if (mode_ == QLatin1String("spectrogram"))
        base_ = Renderer::spectrogram(*audio_, w, h);
    else
        base_ = Renderer::waveform(*audio_, w, h);
}

void AudioView::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    // The cached base image is rendered at widget resolution; rebuild it so a
    // resized view never stretches/stales the old bitmap.
    if (audio_ && audio_->valid) rebuildBaseImage();
    syncScrollBar();
}

void AudioView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QRect content(0, 0, width(), std::max(1, viewContentHeight(this)));
    p.fillRect(rect(), QColor(24, 24, 28));
    if (!audio_ || !audio_->valid) {
        p.setPen(Qt::gray);
        p.drawText(content, Qt::AlignCenter, tr("No audio loaded"));
        return;
    }
    if (base_.isNull()) rebuildBaseImage();
    p.drawImage(content, base_);

    // selection overlay
    if (hasSelection()) {
        int x1 = frameToX(selStart_), x2 = frameToX(selEnd_);
        QRect r(x1, 0, x2 - x1, content.height());
        p.fillRect(r, QColor(80, 140, 255, 70));
        p.setPen(QColor(80, 140, 255));
        p.drawLine(x1, 0, x1, content.height());
        p.drawLine(x2, 0, x2, content.height());
        // edge handles
        p.setBrush(QColor(80, 140, 255));
        p.drawRect(x1 - 3, content.height() / 2 - 8, 6, 16);
        p.drawRect(x2 - 3, content.height() / 2 - 8, 6, 16);
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
            p.drawLine(x, 18, x, content.height());
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
    p.drawLine(px, 0, px, content.height());
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
    // Clicks inside the reserved bottom scrollbar strip belong to the bar
    // itself, not to the view (no selection / cursor move there).
    if (int(e->position().y()) >= viewContentHeight(this)) return;
    int x = e->position().x();
    // selection edges first
    if (hasSelection()) {
        int x1 = frameToX(selStart_), x2 = frameToX(selEnd_);
        if (std::abs(x - x1) <= 5) { drag_ = MoveSelStart; return; }
        if (std::abs(x - x2) <= 5) { drag_ = MoveSelEnd; return; }
    }
    int mi = markerIndexAt(markers_, this, x);
    if (mi >= 0) {
        // Qt delivers press + release BEFORE the double-click event, so a plain
        // click on a marker starts a (possibly zero-distance) MoveMarker drag
        // here. Detect the second consecutive stationary click on the SAME
        // marker directly in the press handler: that is exactly when Qt also
        // sends mouseDoubleClickEvent, so we open the editor once — without
        // relying on fragile timestamp emulation in MainWindow.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool secondClick =
            (e->button() == Qt::LeftButton &&
             dblPressIdx_ == mi && now - dblPressMs_ < QApplication::doubleClickInterval());
        dblPressMs_ = now; dblPressIdx_ = mi;
        activeMarker_ = mi;
        emit markerClicked(mi);
        if (secondClick) {          // paired with mouseDoubleClickEvent below
            drag_ = NoDrag;
            emit markerDoubleClicked(mi);   // main window opens editor dialog
            update();
            return;
        }
        drag_ = MoveMarker;
        dragMarkerIdx_ = mi;
        dragStartX_ = x;
        update();
        return;
    }
    dblPressMs_ = 0; dblPressIdx_ = -1;
    if (e->button() == Qt::LeftButton) {
        drag_ = NewSelection;
        dragStartX_ = x;
        qint64 f = xToFrame(x);
        // Do not create the selection here: a plain click (press + release
        // without dragging) must only move the cursor and clear the
        // selection. The actual selection is finalized in mouseReleaseEvent.
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
    // Only react to movement while a mouse button is held down.
    // With setMouseTracking(true) Qt also delivers hover moves with
    // buttons() == NoButton — those must not create or change a selection.
    if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)))
        return;
    // If no drag was started on press (e.g. button pressed outside the view),
    // ignore plain moves as well.
    if (drag_ == NoDrag)
        return;
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

void AudioView::mouseReleaseEvent(QMouseEvent *e) {
    if (!audio_) { drag_ = NoDrag; return; }
    int x = e->position().x();
    if (drag_ == NewSelection && e->button() == Qt::LeftButton) {
        qint64 a = xToFrame(dragStartX_), b = xToFrame(x);
        if (std::abs(b - a) <= 1) {
            // Plain click without dragging: clear selection, keep cursor.
            clearSelection();
        } else {
            setSelectionRange(a, b);
        }
    } else if ((drag_ == MoveSelStart || drag_ == MoveSelEnd || drag_ == MoveMarker)
               && e->button() == Qt::LeftButton) {
        emit selectionChanged(selStart_, selEnd_);
    }
    drag_ = NoDrag;
    update();
}

void AudioView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (!audio_) return;
    int x = e->position().x();
    qint64 f = xToFrame(x);
    bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    int mi = markerIndexAt(markers_, this, x);
    if (mi >= 0) {
        // Editor dialog opening is handled via the markerDoubleClicked signal
        // emitted from mousePressEvent (Qt sends press before double-click).
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
