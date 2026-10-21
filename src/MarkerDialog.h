#pragma once
#include <QDialog>
#include <QVector>
#include "Types.h"

// Marker editor popup (spec 4.2): editable name, time display, delete button.
class MarkerDialog : public QDialog {
    Q_OBJECT
public:
    MarkerDialog(const Marker &m, int sampleRate, QWidget *parent = nullptr);
    Marker marker() const;
    bool deleteRequested() const { return del_; }

private:
    Marker m_;
    bool del_ = false;
};
