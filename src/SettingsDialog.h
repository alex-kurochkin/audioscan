#pragma once
#include <QDialog>

// Settings dialog (spec 10.4): storage maintenance buttons, undo params,
// preview prefetch, Space behavior, default display modes, hotkey editor.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

signals:
    void hotkeysChanged();

private:
    void buildUi();
};
