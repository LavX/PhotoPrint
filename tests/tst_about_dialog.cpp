#include "AboutDialog.h"
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QtTest>

#include <memory>

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

QLabel *labelNamed(QWidget *parent, const QString &name)
{
    return parent->findChild<QLabel *>(name);
}

/// The version the build was configured with, read from the same declaration
/// that feeds the compile definition, so a stale binary is caught.
QString cmakeProjectVersion()
{
    QFile file(QStringLiteral(PHOTOPRINT_SOURCE_DIR "/CMakeLists.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    static const QRegularExpression pattern(
        QStringLiteral("^project\\(PhotoPrint VERSION ([0-9][0-9.]*)"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(file.readAll()));
    return match.hasMatch() ? match.captured(1) : QString();
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

    QLabel *title = labelNamed(&dialog, QStringLiteral("aboutTitle"));
    QVERIFY2(title, "the title label is missing");
    QCOMPARE(title->text(), QStringLiteral("PhotoPrint"));

    QLabel *copyright = labelNamed(&dialog, QStringLiteral("aboutCopyright"));
    QVERIFY2(copyright, "the copyright label is missing");
    QVERIFY2(QRegularExpression(QStringLiteral("^© \\d{4} \\S+"))
                 .match(copyright->text())
                 .hasMatch(),
             qPrintable(copyright->text()));

    // The credits are a separator list: at least two non-empty components,
    // without pinning the tests to the current third-party versions.
    QLabel *credits = labelNamed(&dialog, QStringLiteral("aboutCredits"));
    QVERIFY2(credits, "the credits label is missing");
    const QStringList components =
        credits->text().split(QStringLiteral("·"), Qt::SkipEmptyParts);
    QVERIFY2(components.size() >= 2, qPrintable(credits->text()));
    for (const QString &component : components)
        QVERIFY2(!component.trimmed().isEmpty(), qPrintable(credits->text()));

    QLabel *licenseTitle = labelNamed(&dialog, QStringLiteral("aboutLicenseTitle"));
    QVERIFY2(licenseTitle, "the licence title label is missing");
    QVERIFY2(licenseTitle->text().contains(QStringLiteral("MIT")),
             qPrintable(licenseTitle->text()));

    // The dialog points at the shipped licence file, so the file has to be the
    // MIT licence it claims to be.
    QFile license(QStringLiteral(PHOTOPRINT_SOURCE_DIR "/LICENSE"));
    QVERIFY2(license.open(QIODevice::ReadOnly | QIODevice::Text), "LICENSE is missing");
    const QString licenseText = QString::fromUtf8(license.readAll());
    QVERIFY2(licenseText.contains(QStringLiteral("MIT License")), "LICENSE is not MIT");
    QVERIFY2(licenseText.contains(QStringLiteral("Permission is hereby granted")),
             "LICENSE has no MIT grant");

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
    const QString expected = cmakeProjectVersion();
    QVERIFY2(!expected.isEmpty(),
             "CMakeLists.txt does not declare project(PhotoPrint VERSION ...)");
    QCOMPARE(AboutDialog::versionString(), expected);
    QVERIFY2(QRegularExpression(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"))
                 .match(AboutDialog::versionString())
                 .hasMatch(),
             qPrintable(AboutDialog::versionString()));

    AboutDialog dialog;
    QLabel *version = labelNamed(&dialog, QStringLiteral("aboutVersion"));
    QVERIFY2(version, "the version label is missing");
    QVERIFY2(version->text().contains(expected), qPrintable(version->text()));
    QVERIFY2(version->text().contains(AboutDialog::versionString()),
             qPrintable(version->text()));
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

    // The probe runs inside the modal event loop. Its state lives on the heap
    // and is captured by value, so nothing dangles if the timer outlives this
    // stack frame.
    struct Probe {
        bool opened = false;
        bool modal = false;
        QString text;
    };
    auto probe = std::make_shared<Probe>();
    QTimer::singleShot(0, qApp, [probe] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        probe->opened = true;
        probe->modal = dialog->isModal();
        probe->text = textOf(dialog);
        dialog->close();
    });
    about->trigger();

    QVERIFY2(probe->opened, "the menu entry did not open a modal dialog");
    QVERIFY(probe->modal);
    QVERIFY2(probe->text.contains(QStringLiteral("PhotoPrint")), qPrintable(probe->text));
    QVERIFY2(probe->text.contains(AboutDialog::versionString()), qPrintable(probe->text));
}

QTEST_MAIN(TestAboutDialog)
#include "tst_about_dialog.moc"
