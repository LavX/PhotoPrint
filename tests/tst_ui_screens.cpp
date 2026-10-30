#include "AboutDialog.h"
#include "MainWindow.h"

#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QAbstractItemView>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QtTest>

using namespace pp;

namespace {

QString evidenceDir()
{
#ifdef PHOTOPRINT_EVIDENCE_DIR
    const QString dir = QStringLiteral(PHOTOPRINT_EVIDENCE_DIR);
#else
    const QString dir = QDir::currentPath() + QStringLiteral("/build/evidence");
#endif
    QDir().mkpath(dir);
    return dir;
}

QString makePhoto(const QString &directory, const QString &name, const QColor &colour, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

} // namespace

class TestUiScreens : public QObject
{
    Q_OBJECT

private slots:
    void captureStates();
};

void TestUiScreens::captureStates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg"),
        makePhoto(dir.path(), QStringLiteral("landscape.png"), QColor(70, 130, 170), QSize(900, 600)),
        makePhoto(dir.path(), QStringLiteral("square.png"), QColor(160, 110, 70), QSize(700, 700)),
    };
    for (const QString &photo : photos)
        QVERIFY2(QFileInfo::exists(photo), qPrintable(photo));

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(150);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-empty.png")));

    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QTest::qWait(450);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-three-photos.png")));

    if (auto *scroll = window.findChild<QScrollArea *>()) {
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTest::qWait(120);
    }
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-controls-bottom.png")));

    if (auto *scroll = window.findChild<QScrollArea *>())
        scroll->verticalScrollBar()->setValue(0);
    QComboBox *paper = nullptr;
    QComboBox *orientation = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->count() > 0 && combo->itemText(0).startsWith(QStringLiteral("A4")))
            paper = combo;
        if (combo->count() == 2 && combo->itemText(0) == QStringLiteral("Álló"))
            orientation = combo;
    }
    QVERIFY(paper);
    QVERIFY(orientation);
    for (int i = 0; i < paper->count(); ++i) {
        if (paper->itemText(i).startsWith(QStringLiteral("10 × 15 cm")))
            paper->setCurrentIndex(i);
    }
    orientation->setCurrentIndex(1);
    QTest::qWait(350);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-photo-paper.png")));

    if (paper) {
        paper->showPopup();
        QTest::qWait(120);
        if (QWidget *popup = paper->view() ? paper->view()->window() : nullptr)
            popup->grab().save(evidenceDir() + QStringLiteral("/ui-paper-popup.png"));
        paper->hidePopup();
    }

    AboutDialog about(&window);
    about.show();
    QTest::qWait(120);
    QVERIFY(about.grab().save(evidenceDir() + QStringLiteral("/ui-dialog.png")));
    about.close();

    for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Új lap")) {
            button->click();
            break;
        }
    }
    QTest::qWait(200);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-pages.png")));

    window.resize(640, 360);
    QTest::qWait(250);
    QVERIFY(window.grab().save(evidenceDir() + QStringLiteral("/ui-small.png")));
}

QTEST_MAIN(TestUiScreens)
#include "tst_ui_screens.moc"
