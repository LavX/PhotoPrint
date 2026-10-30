#include "AboutDialog.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTimer>
#include <QtTest>

using namespace pp;

namespace {

QString textOf(QWidget *widget)
{
    QStringList parts;
    parts << widget->windowTitle();
    for (QLabel *label : widget->findChildren<QLabel *>())
        parts << label->text();
    return parts.join(QLatin1Char(' '));
}

} // namespace

class TestAboutDialog : public QObject
{
    Q_OBJECT

private slots:
    void showsApplicationIdentity();
    void versionComesFromTheBuild();
    void labelsAreAccessible();
    void helpMenuOpensTheDialog();
};

void TestAboutDialog::showsApplicationIdentity()
{
    AboutDialog dialog;
    const QString text = textOf(&dialog);

    QVERIFY2(text.contains(QStringLiteral("PhotoPrint")), qPrintable(text));
    QVERIFY2(text.contains(AboutDialog::versionString()), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("MIT")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("© 2026 Laszlo Toth")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("LICENSE")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Qt 6")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("LittleCMS2")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("ImageMagick")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("CUPS")), qPrintable(text));
    QVERIFY(!dialog.windowTitle().isEmpty());

    QPushButton *close = dialog.findChild<QPushButton *>(QStringLiteral("aboutCloseButton"));
    QVERIFY2(close, "the close button is missing");
    QCOMPARE(dialog.closeButton(), close);
    QVERIFY(!close->text().isEmpty());
    QVERIFY(close->isEnabled());
}

void TestAboutDialog::versionComesFromTheBuild()
{
    // The version is a compile definition from CMake's PROJECT_VERSION.
    QCOMPARE(AboutDialog::versionString(), QStringLiteral(PHOTOPRINT_VERSION));
    QVERIFY(!AboutDialog::versionString().isEmpty());
    QVERIFY(AboutDialog::versionString() != QStringLiteral("0.0.0"));

    AboutDialog dialog;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text().contains(AboutDialog::versionString()))
            return;
    }
    QFAIL("no label carries the build version");
}

void TestAboutDialog::labelsAreAccessible()
{
    AboutDialog dialog;
    const QStringList names = {
        QStringLiteral("aboutIcon"),        QStringLiteral("aboutTitle"),
        QStringLiteral("aboutVersion"),     QStringLiteral("aboutDescription"),
        QStringLiteral("aboutCopyright"),   QStringLiteral("aboutLicenseTitle"),
        QStringLiteral("aboutLicense"),     QStringLiteral("aboutCreditsTitle"),
        QStringLiteral("aboutCredits"),     QStringLiteral("aboutCloseButton"),
    };
    for (const QString &name : names) {
        QWidget *widget = dialog.findChild<QWidget *>(name);
        QVERIFY2(widget, qPrintable(QStringLiteral("%1 is missing").arg(name)));
        QVERIFY2(!widget->accessibleName().isEmpty(),
                 qPrintable(QStringLiteral("%1 has no accessible name").arg(name)));
    }
    QVERIFY(!dialog.accessibleName().isEmpty());
}

void TestAboutDialog::helpMenuOpensTheDialog()
{
    MainWindow window;
    window.show();
    QTest::qWait(50);

    QAction *about = nullptr;
    for (QAction *top : window.menuBar()->actions()) {
        if (!top->menu())
            continue;
        for (QAction *action : top->menu()->actions()) {
            if (action->text() == QStringLiteral("Névjegy"))
                about = action;
        }
    }
    QVERIFY2(about, "Súgó -> Névjegy is missing");

    bool opened = false;
    QString text;
    QTimer::singleShot(0, qApp, [&opened, &text] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        opened = true;
        text = textOf(dialog);
        dialog->close();
    });
    about->trigger();

    QVERIFY2(opened, "the menu entry did not open a modal dialog");
    QVERIFY2(text.contains(QStringLiteral("PhotoPrint")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("© 2026 Laszlo Toth")), qPrintable(text));
}

QTEST_MAIN(TestAboutDialog)
#include "tst_about_dialog.moc"
