#pragma once
#include <QImage>
#include <QWidget>
#include <QVector>
#include "AudioFile.h"
#include "Types.h"

// Visualization area: waveform/spectrogram with zoom, pan, playhead, markers, selection.
class AudioView : public QWidget {
    Q_OBJECT
public:
    explicit AudioView(QWidget *parent = nullptr);

    void setData(const AudioData *audio);           // may be null
    void setMode(const QString &mode);              // "wave" | "spectrogram"
    QString mode() const { return mode_; }
    void setMarkers(const QVector<Marker> *markers);// owned externally
    void setPlayheadFrame(qint64 frame);
    qint64 playheadFrame() const { return cursorFrame_; }
    void setActiveMarkerIndex(int idx);

    int frameToX(qint64 f) const;   // exposed for hit-testing helpers
    qint64 viewStartFrame() const { return viewStart_; }
    qint64 viewLenFrames() const { return viewLen_; }
    qint64 cursorFrame() const { return cursorFrame_; }
    bool hasSelection() const { return selStart_ >= 0 && selEnd_ > selStart_; }
    qint64 selStart() const { return std::max<qint64>(0, selStart_); }
    qint64 selEnd() const { return selEnd_; }
    int activeMarker() const { return activeMarker_; }

    void clearSelection();
    void setSelectionRange(qint64 a, qint64 b);
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void scrollToFrame(qint64 f);

signals:
    void selectionChanged(qint64 start, qint64 end);
    void markerDoubleClickedEmpty(qint64 frame, bool ctrl);   // create marker request
    void doubleClickBetweenMarkers(qint64 a, qint64 b);       // select segment
    void markerClicked(int index);
    void markerDoubleClicked(int index);                       // open editor dialog
    void markerMoved(int index, qint64 newFrame);
    void cursorMoved(qint64 frame);
    void editActionRequested(const QString &action);          // e.g. delete-marker handled by main

public slots:
    void nudgeCursor(int deltaFrames);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    qint64 xToFrame(int x) const;
    void rebuildBaseImage();
    enum DragKind { NoDrag, NewSelection, MoveSelStart, MoveSelEnd, MoveMarker, Pan };
    void applyZoom(double factor, int mouseX);

    const AudioData *audio_ = nullptr;
    const QVector<Marker> *markers_ = nullptr;
    QString mode_ = QStringLiteral("wave");

    QImage base_;               // rendered image at native resolution (frames->px 1:col mapping of full file)
    qint64 viewStart_ = 0;      // first visible frame
    qint64 viewLen_ = 0;        // visible frames (0 => whole file)
    qint64 cursorFrame_ = 0;
    qint64 selStart_ = -1, selEnd_ = -1;
    int activeMarker_ = -1;

    DragKind drag_ = NoDrag;
    int dragStartX_ = 0;
    int dragMarkerIdx_ = -1;
    bool sampleLevel_ = false;  // zoomed to individual samples
    // double-click-on-marker detection (press handler): index + timestamp of
    // the previous press that hit a marker, reset when any other area is hit.
    qint64 dblPressMs_ = 0;
    int dblPressIdx_ = -1;
};
