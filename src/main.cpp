#include <QApplication>
#include <QTimer>
#include <QHeaderView>
#include <QString>
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
#endif

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("audioscout"));
    QCoreApplication::setOrganizationName(QStringLiteral("audioscout"));

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
