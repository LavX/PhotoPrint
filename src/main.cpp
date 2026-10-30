#include "ColorPipeline.h"
#include "Document.h"
#include "ImageSource.h"
#include "LayoutEngine.h"
#include "MainWindow.h"
#include "AboutDialog.h"
#include "PageRenderer.h"
#include "PrinterCatalog.h"
#include "Translation.h"

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PhotoPrint"));
    QApplication::setApplicationDisplayName(QStringLiteral("PhotoPrint"));
    QApplication::setOrganizationName(QStringLiteral("PhotoPrint"));
    QApplication::setDesktopFileName(QStringLiteral("photoprint"));
    QApplication::setApplicationVersion(pp::AboutDialog::versionString());
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/photoprint.png")));

    // The saved choice wins, then the system locale when we ship it, otherwise
    // Hungarian, which is the source language.
    pp::i18n::installLanguage(pp::i18n::selectedLanguage());

    const QStringList arguments = app.arguments();

    if (arguments.contains(QStringLiteral("--startup-probe"))) {
        pp::MainWindow window;
        window.show();
        QTimer::singleShot(350, [] {
            QFile status(QStringLiteral("/proc/self/status"));
            double rssMiB = -1.0;
            if (status.open(QIODevice::ReadOnly)) {
                for (const QByteArray &line : status.readAll().split('\n')) {
                    if (line.startsWith("VmRSS:")) {
                        rssMiB = line.mid(6).trimmed().split(' ').first().toDouble() / 1024.0;
                        break;
                    }
                }
            }
            QJsonObject result;
            result.insert(QStringLiteral("ready"), true);
            result.insert(QStringLiteral("rssMiB"), rssMiB);
            fprintf(stdout, "%s\n",
                    QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            fflush(stdout);
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (arguments.contains(QStringLiteral("--benchmark"))) {
        const int index = arguments.indexOf(QStringLiteral("--benchmark"));
        QString photo = index + 1 < arguments.size() ? arguments.at(index + 1)
                                                     : QString();
        if (photo.isEmpty())
            photo = QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg");

        QElapsedTimer timer;
        timer.start();
        pp::ImageStore store;
        const auto source = store.load(photo, pp::ImportPolicy{});
        if (!source) {
            qCritical().noquote() << source.error().message;
            return 1;
        }
        const QImage preview =
            pp::ColorPipeline::toSrgb(source.value(), pp::ColorRecipe{}, 1000);
        const qint64 previewMs = timer.restart();
        if (preview.isNull())
            return 1;

        pp::DocumentSnapshot document;
        pp::Page page;
        page.id = pp::makeId(QStringLiteral("page"));
        page.paper.name = QStringLiteral("A4");
        page.paper.physicalWidthMm = 210.0;
        page.paper.physicalHeightMm = 297.0;
        pp::Frame frame;
        frame.id = pp::makeId(QStringLiteral("frame"));
        frame.sourcePath = source.value().path;
        frame.sourceSha256 = source.value().sha256;
        frame.cx = 105.0;
        frame.cy = 148.5;
        frame.width = 150.0;
        frame.height = 100.0;
        frame.fit = pp::FitMode::Full;
        page.frames.append(frame);
        document.pages.append(page);

        pp::PageRenderer renderer(store);
        const QString pdfPath = QDir(QStandardPaths::writableLocation(
                                         QStandardPaths::TempLocation))
                                    .filePath(QStringLiteral("photoprint-benchmark.pdf"));
        const auto artifact = renderer.writePdf(document, pdfPath, {},
                                                pp::RenderOptions{});
        const qint64 exportMs = timer.elapsed();
        if (!artifact) {
            qCritical().noquote() << artifact.error().message;
            return 1;
        }

        double rssMiB = -1.0;
        double peakMiB = -1.0;
        QFile status(QStringLiteral("/proc/self/status"));
        if (status.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : status.readAll().split('\n')) {
                if (line.startsWith("VmRSS:"))
                    rssMiB = line.mid(6).trimmed().split(' ').first().toDouble() / 1024.0;
                if (line.startsWith("VmHWM:"))
                    peakMiB = line.mid(6).trimmed().split(' ').first().toDouble() / 1024.0;
            }
        }

        QJsonObject result;
        result.insert(QStringLiteral("firstPreviewMs"), double(previewMs));
        result.insert(QStringLiteral("a4ExportMs"), double(exportMs));
        result.insert(QStringLiteral("rssMiB"), rssMiB);
        result.insert(QStringLiteral("peakMiB"), peakMiB);
        result.insert(QStringLiteral("sourcePixels"),
                      double(source.value().size().width() * source.value().size().height()));
        fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        fflush(stdout);
        QFile::remove(pdfPath);
        return 0;
    }

    if (arguments.contains(QStringLiteral("--printers"))) {
        pp::PrinterCatalog catalog;
        const auto result = catalog.refresh();
        if (!result) {
            qCritical().noquote() << result.error().message;
            return 1;
        }
        for (const pp::PrinterCapabilities &capabilities : result.value()) {
            const QJsonDocument document(pp::capabilitiesToJson(capabilities));
            fprintf(stdout, "%s\n", document.toJson(QJsonDocument::Compact).constData());
        }
        return 0;
    }

    pp::MainWindow window;
    window.show();
    return app.exec();
}
