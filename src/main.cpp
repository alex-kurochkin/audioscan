#include <QApplication>
#include <QTimer>
#include <QHeaderView>
#include <QString>
#include "AudioDebug.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Database.h"

#ifdef AUDIOSCOUT_TESTING
// Dev-only self-test: simulates the user dragging a header section and then
// closing the window, so we can verify the column layout persists. Invoked
// with env AUDIOSCOUT_SELFCOLUMNS=<logical>:<newVisual>, e.g. "0:3".
static void runColumnSelfTest(MainWindow &w) {
    const QString spec = QString::fromLocal8Bit(qgetenv("AUDIOSCOUT_SELFCOLUMNS"));
    QTimer::singleShot(2500, [&w, spec] {
        auto *hdr = w.testTable()->horizontalHeader();
        if (!spec.isEmpty()) {
            const int logical = spec.section(':', 0, 0).toInt();
            const int visual  = spec.section(':', 1, 1).toInt();
            hdr->moveSection(hdr->visualIndex(logical), visual);
        }
        QMetaObject::invokeMethod(&w, "close", Qt::QueuedConnection);
    });
}

// Dev-only: drives the "Columns" dialog like a user — unchecks one column,
// resizes another, clicks Save, then closes the app. Verifies that the
// visibility/order/width choices persist across restarts.
//   AUDIOSCOUT_COLSAVE=<uncheckTitle>:<resizeLogical>:<newWidth>
static void runColumnsDialogSaveTest(MainWindow &w) {
    const QString spec = QString::fromLocal8Bit(qgetenv("AUDIOSCOUT_COLSAVE"));
    QTimer::singleShot(2500, [&w, spec] {
        const QString uncheckTitle = spec.section(':', 0, 0);
        const int resizeLogical = spec.section(':', 1, 1).toInt();
        const int newWidth = spec.section(':', 2, 2).toInt();

        w.openColumnsDialog();   // opens the modal dialog (exec())
        QEventLoop loop;
        ColumnsDialog *dlg = nullptr;
        for (int i = 0; i < 60 && !dlg; ++i) {
            QTimer::singleShot(50, &loop, [&] {
                dlg = w.findChild<ColumnsDialog *>();
                if (dlg) loop.quit();
            });
            loop.processEvents();
        }
        if (!dlg) { qWarning("TEST: Columns dialog not found"); return; }
        const auto boxes = dlg->findChildren<QCheckBox *>();
        bool toggled = false;
        for (auto *cb : boxes)
            if (cb->text().compare(uncheckTitle, Qt::CaseInsensitive) == 0) {
                cb->setChecked(false); toggled = true;
            }
        if (!toggled) qWarning("TEST: checkbox '%s' not found", qPrintable(uncheckTitle));
        if (resizeLogical >= 0 && newWidth > 0)
            w.testTable()->setColumnWidth(resizeLogical, newWidth);
        // Click Save in the dialog's button box.
        bool saved = false;
        for (auto *bb : dlg->findChildren<QDialogButtonBox *>())
            if (auto *save = bb->button(QDialogButtonBox::Save)) { save->click(); saved = true; }
        qWarning("TEST: dialog save clicked=%d", int(saved));
        QTimer::singleShot(800, [&w] { QMetaObject::invokeMethod(&w, "close", Qt::QueuedConnection); });
    });
}
#endif

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("audioscout"));
    QCoreApplication::setOrganizationName(QStringLiteral("audioscout"));

    // Always-on error logging: every qWarning()/qCritical() and DBG()
    // diagnostic is appended to ~/.config/audioscout/logs/audioscout-<date>.log
    // and mirrored to stderr. Install it before anything can fail (DB open,
    // audio device probing) so the very first messages are captured too.
    audioscout_log::install();
    DBG(QStringLiteral("session start; log file: %1").arg(audioscout_log::logFilePath()));

    if (!Database::instance().open(Settings::instance().dbPath())) {
        qWarning("Failed to open database at %s",
                 qPrintable(Settings::instance().dbPath()));
    }

    MainWindow w;
    w.show();
#ifdef AUDIOSCOUT_TESTING
    if (!qgetenv("AUDIOSCOUT_SELFCOLUMNS").isEmpty() ||
        !qgetenv("AUDIOSCOUT_SELCLOSE").isEmpty())
        runColumnSelfTest(w);
#endif
    return app.exec();
}
