#include <QApplication>
#include "MainWindow.h"
#include "Settings.h"
#include "Database.h"

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
    return app.exec();
}
