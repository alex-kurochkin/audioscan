#include <QApplication>
#include <QTimer>
#include <QHeaderView>
#include <QString>
#include "AudioDebug.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Database.h"

#ifdef AUDIOSCOUT_TESTING
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QHeaderView>
#include "ColumnLayoutController.h"

// Dev-only self-test: simulates the user dragging a header section and then
// closing the window, so we can verify the column layout persists. Invoked
// with env AUDIOSCOUT_SELFCOLUMNS=<logical>:<newVisual>, e.g. "0:3".
// AUDIOSCOUT_SELCLOSE alone just closes after the delay (persistence check).
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
// The dialog is modal (exec()), so we queue a one-shot timer *before*
// opening it: when the timer fires inside the dialog's event loop we tweak
// its checkboxes and click its Save button — exactly the user path. After
// exec() returns (Accepted), openDialog() has already applied/saved the
// layout, so we just log the resulting visible-column count and close.
static void runColumnsDialogSaveTest(MainWindow &w) {
    const QString spec = QString::fromLocal8Bit(qgetenv("AUDIOSCOUT_COLSAVE"));
    QTimer::singleShot(150, [&w, spec] {
        const QString uncheckTitle = spec.section(':', 0, 0);
        const int resizeLogical = spec.section(':', 1, 1).toInt();
        const int newWidth = spec.section(':', 2, 2).toInt();

        // Find the live modal dialog (created by openColumnsDialogForTest()).
        ColumnsDialog *dlg = nullptr;
        const auto kids = QApplication::topLevelWidgets();
        for (auto *wd : kids)
            if ((dlg = qobject_cast<ColumnsDialog *>(wd))) break;
        if (!dlg) { qWarning("TEST: Columns dialog not found"); return; }

        bool toggled = false;
        const auto boxes = dlg->findChildren<QCheckBox *>();
        for (auto *cb : boxes)
            if (cb->text().compare(uncheckTitle, Qt::CaseInsensitive) == 0) {
                cb->setChecked(false); toggled = true;
            }
        if (!toggled) qWarning("TEST: checkbox '%s' not found", qPrintable(uncheckTitle));
        if (resizeLogical >= 0 && newWidth > 0)
            w.testTable()->setColumnWidth(resizeLogical, newWidth);
        if (auto *save = dlg->saveButtonForTest())
            QMetaObject::invokeMethod(save, "click", Qt::QueuedConnection);
    });

    const int before = w.testModel()->visibleColumns().size();
    const bool accepted = w.openColumnsDialogForTest();   // blocks until closed
    qWarning("TEST: dialog %s; visible columns %d -> %d",
             accepted ? "accepted" : "rejected",
             before, w.testModel()->visibleColumns().size());
    QTimer::singleShot(800, [&w] { QMetaObject::invokeMethod(&w, "close", Qt::QueuedConnection); });
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
    if (!qgetenv("AUDIOSCOUT_COLSAVE").isEmpty())
        runColumnsDialogSaveTest(w);
#endif
    return app.exec();
}
