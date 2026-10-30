#include "LegacyPpdReader.h"
#include "PrintTicket.h"
#include "PrinterCatalog.h"

#include <cups/cups.h>

#include <QFile>
#include <QtTest>

using namespace pp;

namespace {

// The exported libcups setter the code under test uses to clear a sticky error;
// CUPS 2.x keeps it out of its public headers.
extern "C" void _cupsSetError(ipp_status_t status, const char *message, int localize);

QString fixtureDir()
{
#ifdef PHOTOPRINT_SOURCE_DIR
    return QStringLiteral(PHOTOPRINT_SOURCE_DIR) + QStringLiteral("/tests/fixtures/ppd");
#else
    return QStringLiteral("tests/fixtures/ppd");
#endif
}

PrinterCapabilities nativeCaps()
{
    const auto parsed = PrinterCatalog::fromPpdFile(
        QStringLiteral("DCPT730DW"), fixtureDir() + QStringLiteral("/brother-native.ppd"));
    return parsed.ok() ? parsed.value() : PrinterCapabilities{};
}

PrinterCapabilities ippCaps()
{
    const auto parsed = PrinterCatalog::fromPpdFile(
        QStringLiteral("DCP-T730DW"), fixtureDir() + QStringLiteral("/ipp-everywhere.ppd"));
    return parsed.ok() ? parsed.value() : PrinterCapabilities{};
}

PrinterCapabilities genericCaps()
{
    const auto parsed = PrinterCatalog::fromPpdFile(
        QStringLiteral("Generic"), fixtureDir() + QStringLiteral("/generic-resolution.ppd"));
    return parsed.ok() ? parsed.value() : PrinterCapabilities{};
}

PrintSelection photoSelection()
{
    PrintSelection selection;
    selection.paper.name = QStringLiteral("10 × 15 cm");
    selection.paper.physicalWidthMm = 100.0;
    selection.paper.physicalHeightMm = 150.0;
    selection.paper.logicalWidthMm = 101.6;
    selection.paper.logicalHeightMm = 152.4;
    selection.paper.type = PaperType::Glossy;
    selection.fineQuality = true;
    selection.color = true;
    return selection;
}

PrintSelection a4Selection()
{
    PrintSelection selection;
    selection.paper.name = QStringLiteral("A4");
    selection.paper.physicalWidthMm = 210.0;
    selection.paper.physicalHeightMm = 297.0;
    return selection;
}

PrintSelection customSelection(double widthMm, double heightMm)
{
    PrintSelection selection;
    selection.paper.name = QStringLiteral("Egyedi");
    selection.paper.physicalWidthMm = widthMm;
    selection.paper.physicalHeightMm = heightMm;
    selection.paper.type = PaperType::Glossy;
    selection.paper.custom = true;
    return selection;
}

} // namespace

class TestPrinterOptions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void nativeDriverHasItsOwnOptions();
    void ippQueueUsesItsOwnCapabilities();
    void twoQueuesStayDistinct();
    void adapterMapsNativePhotoPaper();
    void adapterMapsIppPhotoPaper();
    void unknownDriverGetsNoBrotherOptions();
    void unsupportedBorderlessIsRejected();
    void unknownPaperIsRejected();
    void unknownImageableAreaStaysUnknown();
    void borderlessPhotoPaperUsesBorderlessMedia();
    void smallerPhotoPapersMapToTheirOwnModes();
    void paperTypesChooseTheirOwnMedia();
    void customSizeUsesTheCustomOption();
    void customSizeOutsideTheRangeIsRejected();
    void customBorderlessIsRejected();
    void qualityChoiceMapsToTheHighAndNormalOptions();
    void resolutionGroupIsRead();
    void resolutionFollowsTheQualityLevel();
    void midResolutionQueueStaysAtSixHundred();
    void lowResolutionQueueIsRespected();
    void highResolutionQueueKeepsItsOption();
    void colourChoiceMapsToGrayAndMono();
    void copiesAndPageRangeAreChecked();
    void duplexFollowsTheQueueDefault();
    void halfDeclaredLogicalSizeKeepsOneSheet();
    void destinationLookupNeedsTheStatus();
    void stickyCupsErrorIsClearedBeforeTheLookup();
    void printContextEqualityUsesMillimetreTolerance();
};

void TestPrinterOptions::initTestCase()
{
    QVERIFY(QFile::exists(fixtureDir() + QStringLiteral("/brother-native.ppd")));
    QVERIFY(QFile::exists(fixtureDir() + QStringLiteral("/ipp-everywhere.ppd")));
}

void TestPrinterOptions::nativeDriverHasItsOwnOptions()
{
    const PrinterCapabilities capabilities = nativeCaps();
    QCOMPARE(capabilities.queueName, QStringLiteral("DCPT730DW"));
    QCOMPARE(capabilities.driverIdentity, QStringLiteral("brother-native"));
    QCOMPARE(capabilities.manufacturer, QStringLiteral("Brother"));
    QVERIFY(!capabilities.ppdFingerprint.isEmpty());
    QCOMPARE(capabilities.defaultPageSize, QStringLiteral("BrPostC4x6_S"));
    QCOMPARE(capabilities.defaultMediaType, QStringLiteral("Glossy"));
    QCOMPARE(capabilities.defaultQuality, QStringLiteral("Fine"));
    QCOMPARE(capabilities.defaultColor, QStringLiteral("FullColor"));
    QCOMPARE(capabilities.defaultDuplex, QStringLiteral("None"));
    QVERIFY(capabilities.borderlessSupported());

    const PageSizeCapability *page = capabilities.findPageSize(QStringLiteral("BrPostC4x6_S"));
    QVERIFY(page);
    QVERIFY(std::abs(page->widthMm - 101.6) < 0.05);
    QVERIFY(std::abs(page->heightMm - 152.4) < 0.05);
    QVERIFY(page->imageableKnown);
    QVERIFY(std::abs(page->imageableMm.left() - 3.175) < 0.05);
    QVERIFY(std::abs(page->imageableMm.right() - 98.425) < 0.05);

    const PageSizeCapability *borderless = capabilities.findPageSize(QStringLiteral("BrPostC4x6_B"));
    QVERIFY(borderless);
    QVERIFY(borderless->borderless);
    QVERIFY(borderless->imageableKnown);
    QCOMPARE(borderless->imageableMm.left(), 0.0);
}

void TestPrinterOptions::ippQueueUsesItsOwnCapabilities()
{
    const PrinterCapabilities capabilities = ippCaps();
    QCOMPARE(capabilities.queueName, QStringLiteral("DCP-T730DW"));
    QCOMPARE(capabilities.driverIdentity, QStringLiteral("ipp-everywhere"));
    QCOMPARE(capabilities.defaultPageSize, QStringLiteral("A4"));
    QCOMPARE(capabilities.defaultMediaType, QStringLiteral("Stationery"));
    QCOMPARE(capabilities.defaultQuality, QStringLiteral("Normal"));
    QCOMPARE(capabilities.defaultColor, QStringLiteral("RGB"));

    const PageSizeCapability *page = capabilities.findPageSize(QStringLiteral("4x6"));
    QVERIFY(page);
    QVERIFY(std::abs(page->widthMm - 101.6) < 0.05);
    QVERIFY(std::abs(page->heightMm - 152.4) < 0.05);
    QVERIFY(page->imageableKnown);

    bool hasGlossy = false;
    for (const MediaTypeCapability &type : capabilities.mediaTypes) {
        if (type.photo && type.option.contains(QStringLiteral("Glossy")))
            hasGlossy = true;
    }
    QVERIFY(hasGlossy);
}

void TestPrinterOptions::twoQueuesStayDistinct()
{
    const PrinterCapabilities native = nativeCaps();
    const PrinterCapabilities ipp = ippCaps();
    QVERIFY(native.queueName != ipp.queueName);
    QVERIFY(native.driverIdentity != ipp.driverIdentity);
    QVERIFY(native.ppdFingerprint != ipp.ppdFingerprint);
    QVERIFY(native.pageSizes.size() != ipp.pageSizes.size());
}

void TestPrinterOptions::adapterMapsNativePhotoPaper()
{
    const auto ticket = PrinterAdapter::resolve(nativeCaps(), photoSelection());
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    const PrintTicket &value = ticket.value();
    QCOMPARE(value.pageSizeOption, QStringLiteral("BrPostC4x6_S"));
    QCOMPARE(value.mediaTypeOption, QStringLiteral("Glossy"));
    QCOMPARE(value.qualityOption, QStringLiteral("Fine"));
    QCOMPARE(value.colorOption, QStringLiteral("FullColor"));
    QCOMPARE(value.duplexOption, QStringLiteral("None"));
    QCOMPARE(value.geometry.landscape, false);
    QVERIFY(std::abs(value.geometry.logicalSizeMm().width() - 101.6) < 0.05);
    QVERIFY(std::abs(value.geometry.logicalSizeMm().height() - 152.4) < 0.05);
    QVERIFY(value.geometry.imageableKnown);
    QVERIFY(!value.geometry.offsetVerified);
}

void TestPrinterOptions::adapterMapsIppPhotoPaper()
{
    const auto ticket = PrinterAdapter::resolve(ippCaps(), photoSelection());
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    const PrintTicket &value = ticket.value();
    QCOMPARE(value.pageSizeOption, QStringLiteral("4x6"));
    QCOMPARE(value.mediaTypeOption, QStringLiteral("PhotographicGlossy"));
    QCOMPARE(value.qualityOption, QStringLiteral("High"));
    QCOMPARE(value.colorOption, QStringLiteral("RGB"));
    QCOMPARE(value.duplexOption, QStringLiteral("None"));
}

void TestPrinterOptions::unknownDriverGetsNoBrotherOptions()
{
    PrinterCapabilities capabilities = ippCaps();
    capabilities.driverIdentity = QStringLiteral("unknown");
    capabilities.mediaTypes.clear();
    capabilities.qualityOptions.clear();
    capabilities.colorOptions.clear();
    capabilities.duplexOptions.clear();
    capabilities.defaultMediaType.clear();
    capabilities.defaultQuality.clear();
    capabilities.defaultColor.clear();
    capabilities.defaultDuplex.clear();

    const auto ticket = PrinterAdapter::resolve(capabilities, photoSelection());
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    QCOMPARE(ticket.value().pageSizeOption, QStringLiteral("4x6"));
    QVERIFY(ticket.value().mediaTypeOption.isEmpty());
    QVERIFY(ticket.value().qualityOption.isEmpty());
    QVERIFY(ticket.value().colorOption.isEmpty());
    QVERIFY(!ticket.value().pageSizeOption.contains(QStringLiteral("Br")));
}

void TestPrinterOptions::unsupportedBorderlessIsRejected()
{
    PrinterCapabilities capabilities = ippCaps();
    for (int i = capabilities.pageSizes.size() - 1; i >= 0; --i) {
        if (capabilities.pageSizes.at(i).borderless)
            capabilities.pageSizes.removeAt(i);
    }
    PrintSelection selection = photoSelection();
    selection.borderless = true;
    const auto ticket = PrinterAdapter::resolve(capabilities, selection);
    QVERIFY(!ticket.ok());
    QCOMPARE(ticket.error().code, QStringLiteral("borderless-unsupported"));
}

void TestPrinterOptions::unknownPaperIsRejected()
{
    PrintSelection selection = a4Selection();
    selection.paper.physicalWidthMm = 200.0;
    selection.paper.physicalHeightMm = 300.0;
    selection.paper.name = QStringLiteral("200 × 300 mm");
    const auto ticket = PrinterAdapter::resolve(nativeCaps(), selection);
    QVERIFY(!ticket.ok());
    QCOMPARE(ticket.error().code, QStringLiteral("paper-unsupported"));
}

void TestPrinterOptions::unknownImageableAreaStaysUnknown()
{
    PrinterCapabilities capabilities = nativeCaps();
    for (PageSizeCapability &page : capabilities.pageSizes) {
        if (page.option == QLatin1String("BrPostC4x6_S")) {
            page.imageableKnown = false;
            page.imageableMm = QRectF();
        }
    }
    const auto ticket = PrinterAdapter::resolve(capabilities, photoSelection());
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    QVERIFY(!ticket.value().geometry.imageableKnown);
}

void TestPrinterOptions::borderlessPhotoPaperUsesBorderlessMedia()
{
    PrintSelection selection = photoSelection();
    selection.borderless = true;

    const auto native = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!native)
        QFAIL(qPrintable(native.error().message));
    QCOMPARE(native.value().pageSizeOption, QStringLiteral("BrPostC4x6_B"));
    QVERIFY(native.value().geometry.borderless);
    QVERIFY(std::abs(native.value().geometry.logicalSizeMm().width() - 101.6) < 0.05);

    const auto ipp = PrinterAdapter::resolve(ippCaps(), selection);
    if (!ipp)
        QFAIL(qPrintable(ipp.error().message));
    QCOMPARE(ipp.value().pageSizeOption, QStringLiteral("4x6.Borderless"));
    QVERIFY(ipp.value().geometry.borderless);
}

void TestPrinterOptions::smallerPhotoPapersMapToTheirOwnModes()
{
    PrintSelection nine = photoSelection();
    nine.paper.name = QStringLiteral("9 × 13 cm");
    nine.paper.physicalWidthMm = 88.9;
    nine.paper.physicalHeightMm = 127.0;
    nine.paper.logicalWidthMm = 88.9;
    nine.paper.logicalHeightMm = 127.0;
    nine.borderless = true;
    const auto nineNative = PrinterAdapter::resolve(nativeCaps(), nine);
    if (!nineNative)
        QFAIL(qPrintable(nineNative.error().message));
    QCOMPARE(nineNative.value().pageSizeOption, QStringLiteral("BrPhotoL_B"));
    const auto nineIpp = PrinterAdapter::resolve(ippCaps(), nine);
    if (!nineIpp)
        QFAIL(qPrintable(nineIpp.error().message));
    QCOMPARE(nineIpp.value().pageSizeOption, QStringLiteral("3.5x5.Borderless"));

    PrintSelection thirteen = nine;
    thirteen.paper.name = QStringLiteral("13 × 18 cm");
    thirteen.paper.physicalWidthMm = 127.0;
    thirteen.paper.physicalHeightMm = 177.8;
    thirteen.paper.logicalWidthMm = 127.0;
    thirteen.paper.logicalHeightMm = 177.8;
    const auto thirteenNative = PrinterAdapter::resolve(nativeCaps(), thirteen);
    if (!thirteenNative)
        QFAIL(qPrintable(thirteenNative.error().message));
    QCOMPARE(thirteenNative.value().pageSizeOption, QStringLiteral("BrPhoto2L_B"));
    const auto thirteenIpp = PrinterAdapter::resolve(ippCaps(), thirteen);
    if (!thirteenIpp)
        QFAIL(qPrintable(thirteenIpp.error().message));
    QCOMPARE(thirteenIpp.value().pageSizeOption, QStringLiteral("5x7.Borderless"));

    thirteen.borderless = false;
    const auto bordered = PrinterAdapter::resolve(nativeCaps(), thirteen);
    if (!bordered)
        QFAIL(qPrintable(bordered.error().message));
    QCOMPARE(bordered.value().pageSizeOption, QStringLiteral("BrPhoto2L_S"));
}

void TestPrinterOptions::paperTypesChooseTheirOwnMedia()
{
    const PrintSelection glossy = photoSelection();

    const auto glossyNative = PrinterAdapter::resolve(nativeCaps(), glossy);
    if (!glossyNative)
        QFAIL(qPrintable(glossyNative.error().message));
    QCOMPARE(glossyNative.value().mediaTypeOption, QStringLiteral("Glossy"));

    const auto glossyIpp = PrinterAdapter::resolve(ippCaps(), glossy);
    if (!glossyIpp)
        QFAIL(qPrintable(glossyIpp.error().message));
    QCOMPARE(glossyIpp.value().mediaTypeOption, QStringLiteral("PhotographicGlossy"));

    PrintSelection matte = glossy;
    matte.paper.type = PaperType::Matte;
    const auto matteNative = PrinterAdapter::resolve(nativeCaps(), matte);
    if (!matteNative)
        QFAIL(qPrintable(matteNative.error().message));
    QCOMPARE(matteNative.value().mediaTypeOption, QStringLiteral("Inkjet"));
    const auto matteIpp = PrinterAdapter::resolve(ippCaps(), matte);
    if (!matteIpp)
        QFAIL(qPrintable(matteIpp.error().message));
    QCOMPARE(matteIpp.value().mediaTypeOption, QStringLiteral("StationeryInkjet"));

    const PrintSelection plain = a4Selection();
    const auto plainNative = PrinterAdapter::resolve(nativeCaps(), plain);
    if (!plainNative)
        QFAIL(qPrintable(plainNative.error().message));
    QCOMPARE(plainNative.value().mediaTypeOption, QStringLiteral("Plain"));
    const auto plainIpp = PrinterAdapter::resolve(ippCaps(), plain);
    if (!plainIpp)
        QFAIL(qPrintable(plainIpp.error().message));
    QCOMPARE(plainIpp.value().mediaTypeOption, QStringLiteral("Stationery"));
}

void TestPrinterOptions::customSizeUsesTheCustomOption()
{
    const PrintSelection selection = customSelection(120.0, 180.0);

    const auto native = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!native)
        QFAIL(qPrintable(native.error().message));
    QCOMPARE(native.value().pageSizeOption, QStringLiteral("Custom.120x180mm"));
    QCOMPARE(native.value().mediaTypeOption, QStringLiteral("Glossy"));
    QVERIFY(!native.value().geometry.borderless);
    QVERIFY(std::abs(native.value().geometry.logicalSizeMm().width() - 120.0) < 0.05);
    QVERIFY(std::abs(native.value().geometry.logicalSizeMm().height() - 180.0) < 0.05);

    const auto ipp = PrinterAdapter::resolve(ippCaps(), selection);
    if (!ipp)
        QFAIL(qPrintable(ipp.error().message));
    QCOMPARE(ipp.value().pageSizeOption, QStringLiteral("Custom.120x180mm"));
    QVERIFY(std::abs(ipp.value().geometry.physicalSizeMm().width() - 120.0) < 0.05);

    QVERIFY(PrinterAdapter::customSizeInRange(120.0, 180.0));
    QVERIFY(PrinterAdapter::customSizeInRange(88.9, 127.0));
    QVERIFY(!PrinterAdapter::customSizeInRange(50.0, 180.0));
    QVERIFY(!PrinterAdapter::customSizeInRange(250.0, 250.0));
}

void TestPrinterOptions::customSizeOutsideTheRangeIsRejected()
{
    const auto tooNarrow = PrinterAdapter::resolve(nativeCaps(), customSelection(50.0, 400.0));
    QVERIFY(!tooNarrow.ok());
    QCOMPARE(tooNarrow.error().code, QStringLiteral("custom-size-range"));

    const auto tooWide = PrinterAdapter::resolve(ippCaps(), customSelection(250.0, 250.0));
    QVERIFY(!tooWide.ok());
    QCOMPARE(tooWide.error().code, QStringLiteral("custom-size-range"));
}

void TestPrinterOptions::customBorderlessIsRejected()
{
    PrintSelection selection = customSelection(120.0, 180.0);
    selection.borderless = true;
    const auto ticket = PrinterAdapter::resolve(nativeCaps(), selection);
    QVERIFY(!ticket.ok());
    QCOMPARE(ticket.error().code, QStringLiteral("borderless-unsupported"));

    // A user-defined sheet that is exactly 10 × 15 cm still uses the fixed,
    // verified borderless mode instead of the uncertain custom borderless.
    PrintSelection matching = customSelection(100.0, 150.0);
    matching.borderless = true;
    const auto matchingTicket = PrinterAdapter::resolve(nativeCaps(), matching);
    if (!matchingTicket)
        QFAIL(qPrintable(matchingTicket.error().message));
    QCOMPARE(matchingTicket.value().pageSizeOption, QStringLiteral("BrPostC4x6_B"));
    QVERIFY(matchingTicket.value().geometry.borderless);
}

void TestPrinterOptions::qualityChoiceMapsToTheHighAndNormalOptions()
{
    PrintSelection selection = photoSelection();
    selection.fineQuality = true;

    const auto fineNative = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!fineNative)
        QFAIL(qPrintable(fineNative.error().message));
    QCOMPARE(fineNative.value().qualityOption, QStringLiteral("Fine"));

    const auto fineIpp = PrinterAdapter::resolve(ippCaps(), selection);
    if (!fineIpp)
        QFAIL(qPrintable(fineIpp.error().message));
    QCOMPARE(fineIpp.value().qualityOption, QStringLiteral("High"));

    selection.fineQuality = false;
    const auto normalNative = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!normalNative)
        QFAIL(qPrintable(normalNative.error().message));
    QCOMPARE(normalNative.value().qualityOption, QStringLiteral("Normal"));

    const auto normalIpp = PrinterAdapter::resolve(ippCaps(), selection);
    if (!normalIpp)
        QFAIL(qPrintable(normalIpp.error().message));
    QCOMPARE(normalIpp.value().qualityOption, QStringLiteral("Normal"));
}

void TestPrinterOptions::resolutionGroupIsRead()
{
    const PrinterCapabilities capabilities = genericCaps();
    QCOMPARE(capabilities.resolutionOptionName, QStringLiteral("Resolution"));
    const QStringList expected = {QStringLiteral("300dpi"), QStringLiteral("600dpi"),
                                  QStringLiteral("1200dpi")};
    QCOMPARE(capabilities.resolutionOptions, expected);
    QCOMPARE(capabilities.defaultResolution, QStringLiteral("600dpi"));
    // Resolution keywords must not leak into the quality list: those are
    // cupsPrintQuality values, and sending 600dpi as one would be invalid.
    QVERIFY(!capabilities.qualityOptions.contains(QStringLiteral("600dpi")));
}

void TestPrinterOptions::resolutionFollowsTheQualityLevel()
{
    const PrinterCapabilities capabilities = genericCaps();

    const auto fine = PrinterAdapter::resolve(capabilities, photoSelection());
    QVERIFY(fine.ok());
    QCOMPARE(fine.value().printResolutionDpi, 1200);
    QCOMPARE(fine.value().resolutionOption, QStringLiteral("Resolution"));
    QCOMPARE(fine.value().resolutionValue, QStringLiteral("1200dpi"));

    PrintSelection normal = photoSelection();
    normal.fineQuality = false;
    const auto everyday = PrinterAdapter::resolve(capabilities, normal);
    QVERIFY(everyday.ok());
    QCOMPARE(everyday.value().printResolutionDpi, 300);
    QCOMPARE(everyday.value().resolutionValue, QStringLiteral("300dpi"));
}

void TestPrinterOptions::midResolutionQueueStaysAtSixHundred()
{
    PrinterCapabilities capabilities = genericCaps();
    capabilities.resolutionOptions = {QStringLiteral("300dpi"), QStringLiteral("600dpi")};
    const auto ticket = PrinterAdapter::resolve(capabilities, photoSelection());
    QVERIFY(ticket.ok());
    QCOMPARE(ticket.value().printResolutionDpi, 600);
    QCOMPARE(ticket.value().resolutionValue, QStringLiteral("600dpi"));
}

void TestPrinterOptions::lowResolutionQueueIsRespected()
{
    PrinterCapabilities capabilities = genericCaps();
    capabilities.resolutionOptions = {QStringLiteral("300dpi")};
    const auto ticket = PrinterAdapter::resolve(capabilities, photoSelection());
    QVERIFY(ticket.ok());
    QCOMPARE(ticket.value().printResolutionDpi, 300);
    QCOMPARE(ticket.value().resolutionValue, QStringLiteral("300dpi"));
}

void TestPrinterOptions::highResolutionQueueKeepsItsOption()
{
    PrinterCapabilities capabilities = genericCaps();
    capabilities.resolutionOptions = {QStringLiteral("1200dpi")};
    PrintSelection normal = photoSelection();
    normal.fineQuality = false;
    const auto ticket = PrinterAdapter::resolve(capabilities, normal);
    QVERIFY(ticket.ok());
    // The queue has no lower option, so the keyword stays 1200dpi; the raster
    // itself is still capped at 600 ppi by JobController.
    QCOMPARE(ticket.value().printResolutionDpi, 1200);
    QCOMPARE(ticket.value().resolutionValue, QStringLiteral("1200dpi"));
}

void TestPrinterOptions::colourChoiceMapsToGrayAndMono()
{
    PrintSelection selection = photoSelection();
    selection.color = true;
    const auto colourNative = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!colourNative)
        QFAIL(qPrintable(colourNative.error().message));
    QCOMPARE(colourNative.value().colorOption, QStringLiteral("FullColor"));

    selection.color = false;
    const auto grayNative = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!grayNative)
        QFAIL(qPrintable(grayNative.error().message));
    QCOMPARE(grayNative.value().colorOption, QStringLiteral("Mono"));

    const auto grayIpp = PrinterAdapter::resolve(ippCaps(), selection);
    if (!grayIpp)
        QFAIL(qPrintable(grayIpp.error().message));
    QCOMPARE(grayIpp.value().colorOption, QStringLiteral("Gray"));
}

void TestPrinterOptions::copiesAndPageRangeAreChecked()
{
    PrintSelection selection = photoSelection();
    selection.copies = 0;
    QVERIFY(!PrinterAdapter::resolve(nativeCaps(), selection).ok());

    selection.copies = 2;
    selection.firstPage = 3;
    selection.lastPage = 1;
    const auto ticket = PrinterAdapter::resolve(nativeCaps(), selection);
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    QCOMPARE(ticket.value().copies, 2);
    QCOMPARE(ticket.value().firstPage, 3);
    QCOMPARE(ticket.value().lastPage, -1);
}

void TestPrinterOptions::duplexFollowsTheQueueDefault()
{
    // A queue that exposes a Duplex group but no "None" keyword makes the
    // driver default the resolved value; the adapter must not invent "None".
    PrinterCapabilities capabilities = nativeCaps();
    capabilities.duplexOptions = {QStringLiteral("DuplexTumble"),
                                  QStringLiteral("DuplexNoTumble")};
    capabilities.defaultDuplex = QStringLiteral("DuplexTumble");

    const auto ticket = PrinterAdapter::resolve(capabilities, photoSelection());
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    QCOMPARE(ticket.value().duplexOption, QStringLiteral("DuplexTumble"));

    // The ordinary queue keeps resolving to single-sided.
    const auto everyday = PrinterAdapter::resolve(nativeCaps(), photoSelection());
    if (!everyday)
        QFAIL(qPrintable(everyday.error().message));
    QCOMPARE(everyday.value().duplexOption, QStringLiteral("None"));
}

void TestPrinterOptions::halfDeclaredLogicalSizeKeepsOneSheet()
{
    // One logical axis declared, one not: the driver page must fall back to the
    // physical sheet as a whole, exactly like the PDF media box does. Falling
    // back per axis asked for a 101.6 x 145 mm page (a sheet that exists
    // nowhere) while the media box described 95 x 145 mm.
    PrinterCapabilities capabilities;
    capabilities.queueName = QStringLiteral("Test");

    PageSizeCapability hybrid;
    hybrid.option = QStringLiteral("Hybrid");
    hybrid.displayName = hybrid.option;
    hybrid.widthMm = 101.6;
    hybrid.heightMm = 145.0;
    capabilities.pageSizes.append(hybrid);

    PageSizeCapability physical;
    physical.option = QStringLiteral("Physical");
    physical.displayName = physical.option;
    physical.widthMm = 95.0;
    physical.heightMm = 145.0;
    capabilities.pageSizes.append(physical);

    PrintSelection selection;
    selection.queueName = capabilities.queueName;
    selection.paper.name = QStringLiteral("Félig megadott");
    selection.paper.physicalWidthMm = 95.0;
    selection.paper.physicalHeightMm = 145.0;
    selection.paper.logicalWidthMm = 101.6;
    selection.paper.logicalHeightMm = 0.0;

    const auto ticket = PrinterAdapter::resolve(capabilities, selection);
    if (!ticket)
        QFAIL(qPrintable(ticket.error().message));
    QCOMPARE(ticket.value().pageSizeOption, QStringLiteral("Physical"));
    QCOMPARE(ticket.value().geometry.logicalSizeMm(), QSizeF(95.0, 145.0));

    // The driver option and the media box must name the same sheet.
    const PageSizeCapability *chosen =
        capabilities.findPageSize(ticket.value().pageSizeOption);
    QVERIFY(chosen);
    QCOMPARE(QSizeF(chosen->widthMm, chosen->heightMm),
             ticket.value().geometry.logicalSizeMm());

    // A fully declared logical sheet is still what the driver gets.
    selection.paper.logicalHeightMm = 145.0;
    const auto declared = PrinterAdapter::resolve(capabilities, selection);
    if (!declared)
        QFAIL(qPrintable(declared.error().message));
    QCOMPARE(declared.value().pageSizeOption, QStringLiteral("Hybrid"));
    QCOMPARE(declared.value().geometry.logicalSizeMm(), QSizeF(101.6, 145.0));
}

void TestPrinterOptions::destinationLookupNeedsTheStatus()
{
    // libcups answers "cannot reach the scheduler" with the same zero count as
    // "no queues configured"; only the status separates the two answers.
    QCOMPARE(int(classifyDestinationLookup(2, int(IPP_STATUS_OK))),
             int(LookupAnswer::Printers));
    QCOMPARE(int(classifyDestinationLookup(2, int(IPP_STATUS_ERROR_INTERNAL))),
             int(LookupAnswer::Printers));
    QCOMPARE(int(classifyDestinationLookup(0, int(IPP_STATUS_OK))),
             int(LookupAnswer::NoPrinters));
    QCOMPARE(int(classifyDestinationLookup(0, int(IPP_STATUS_ERROR_INTERNAL))),
             int(LookupAnswer::Unavailable));
    QCOMPARE(int(classifyDestinationLookup(0, int(IPP_STATUS_ERROR_SERVICE_UNAVAILABLE))),
             int(LookupAnswer::Unavailable));
    QCOMPARE(int(classifyDestinationLookup(-1, int(IPP_STATUS_OK))),
             int(LookupAnswer::Unavailable));
    QCOMPARE(int(classifyDestinationLookup(-1, int(IPP_STATUS_ERROR_INTERNAL))),
             int(LookupAnswer::Unavailable));
}

void TestPrinterOptions::stickyCupsErrorIsClearedBeforeTheLookup()
{
    // libcups keeps the last error for the life of the process, and it survives
    // a successful call, so an empty enumeration must not be classified against
    // a failure left behind earlier. No scheduler is needed to see this.
    clearCupsError();
    QCOMPARE(int(cupsLastError()), int(IPP_STATUS_OK));
    QCOMPARE(int(classifyDestinationLookup(0, int(cupsLastError()))),
             int(LookupAnswer::NoPrinters));

    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, "stale failure", 0);
    QVERIFY2(int(cupsLastError()) != int(IPP_STATUS_OK), "the error is meant to be sticky");
    QCOMPARE(int(classifyDestinationLookup(0, int(cupsLastError()))),
             int(LookupAnswer::Unavailable));

    clearCupsError();
    QCOMPARE(int(cupsLastError()), int(IPP_STATUS_OK));
    QCOMPARE(int(classifyDestinationLookup(0, int(cupsLastError()))),
             int(LookupAnswer::NoPrinters));
}

void TestPrinterOptions::printContextEqualityUsesMillimetreTolerance()
{
    PrintContext context;
    context.queueName = QStringLiteral("DCPT730DW");
    context.physicalWidthMm = 101.6;
    context.physicalHeightMm = 152.4;
    context.logicalWidthMm = 101.6;
    context.logicalHeightMm = 152.4;

    PrintContext same = context;
    same.physicalWidthMm += 0.0041; // PPD round-trip noise, below 0.01 mm
    same.logicalHeightMm -= 0.0041;
    QVERIFY(same == context);

    PrintContext different = context;
    different.physicalWidthMm += 0.02;
    QVERIFY(!(different == context));

    PrintContext otherQueue = context;
    otherQueue.queueName = QStringLiteral("DCP-T730DW");
    QVERIFY(!(otherQueue == context));
}

QTEST_GUILESS_MAIN(TestPrinterOptions)
#include "tst_printer_options.moc"
