#include "CollageTemplates.h"

#include "LayoutEngine.h"
#include "PaperCatalog.h"
#include "PaperGeometry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace pp;

namespace {

const QString kInvalid = QStringLiteral("collage-catalogue-invalid");
const QString kTooSmall = QStringLiteral("template-too-small");

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

QByteArray shippedJson()
{
    QFile file(QStringLiteral(":/templates/collage.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QJsonObject shippedRoot()
{
    return QJsonDocument::fromJson(shippedJson()).object();
}

QByteArray encoded(const QJsonObject &root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonArray templatesOf(const QJsonObject &root)
{
    return root.value(QStringLiteral("templates")).toArray();
}

QJsonObject withTemplates(QJsonObject root, const QJsonArray &templates)
{
    root.insert(QStringLiteral("templates"), templates);
    return root;
}

QJsonObject templateAt(const QJsonObject &root, int index)
{
    return templatesOf(root).at(index).toObject();
}

QJsonObject replaceTemplate(QJsonObject root, int index, const QJsonObject &entry)
{
    QJsonArray entries = templatesOf(root);
    entries.replace(index, entry);
    return withTemplates(root, entries);
}

QJsonObject replaceSlot(QJsonObject root, int templateIndex, int slotIndex, const QJsonObject &slot)
{
    QJsonObject entry = templateAt(root, templateIndex);
    QJsonArray slotArray = entry.value(QStringLiteral("slots")).toArray();
    slotArray.replace(slotIndex, slot);
    entry.insert(QStringLiteral("slots"), slotArray);
    return replaceTemplate(root, templateIndex, entry);
}

QJsonArray rectOf(const QJsonObject &slot)
{
    return slot.value(QStringLiteral("rect")).toArray();
}

QJsonObject withRect(QJsonObject slot, int index, double value)
{
    QJsonArray rect = rectOf(slot);
    rect.replace(index, value);
    slot.insert(QStringLiteral("rect"), rect);
    return slot;
}

PaperSpec paperNamed(const QString &name)
{
    for (const PaperSpec &paper : defaultPapers()) {
        if (paper.name == name)
            return paper;
    }
    return {};
}

/// A frame whose non-geometry fields are all non-default, so a lost field shows
/// up as a failed comparison rather than as a value that happens to match the
/// default.
Frame frameWith(int index, const QString &prefix)
{
    Frame frame;
    frame.id = QStringLiteral("%1-%2").arg(prefix).arg(index);
    frame.sourcePath = QStringLiteral("/photos/%1-%2.jpg").arg(prefix).arg(index);
    frame.sourceSha256 = QByteArrayLiteral("sha-") + QByteArray::number(index);
    frame.cx = 1.0 + index;
    frame.cy = 2.0 + index;
    frame.width = 50.0;
    frame.height = 30.0;
    frame.angleDeg = 12.0;
    frame.imageRotationDeg = 90.0 * index;
    frame.zoom = 1.25;
    frame.cropCenter = QPointF(0.3, 0.4);
    frame.fit = FitMode::Full;
    frame.autoPlaced = true;
    frame.tone.exposureEv = 0.1 * index;
    frame.tone.shadowLift = 0.02 * index;
    frame.tone.highlightCompress = 0.01 * index;
    frame.autoTone = false;
    frame.brightness = 0.05 * (index - 1);
    frame.paperOverride = index % 2 == 0;
    frame.paperEv = 0.12 * (index % 3);
    frame.label = QStringLiteral("#%1").arg(index + 1);
    frame.filter = QStringLiteral("print.sepia");
    frame.filterStrength = 40 + index;
    frame.effectVersion = 2 + index;
    frame.upscale = true;
    return frame;
}

QList<Frame> framesWith(const CollageTemplate &t)
{
    QList<Frame> frames;
    frames.reserve(t.photoCount);
    for (int i = 0; i < t.photoCount; ++i)
        frames.append(frameWith(i, t.id));
    return frames;
}

} // namespace

class TestCollageTemplates : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void catalogueLoads();
    void catalogueOrderIsFillOrder();
    void rejectsMalformedCatalogue();
    void mapsEveryPaperBothWays();
    void usesPhysicalSheetAndIgnoresImageable();
    void refusesSmallPaper();
    void applyPreservesFrameFields();
    void applyCropPolicy();
    void applyThroughPage();
    void applyRefusesWrongCount();
    void applyRefusesMissingSource();
    void addedPhotoDoesNotReflow();
    void labelsAndReferenceNames();
};

void TestCollageTemplates::initTestCase()
{
    // instance() registers the embedded resource; the helpers read it only
    // after this has run.
    QVERIFY(!CollageTemplates::instance().isEmpty());
}

void TestCollageTemplates::catalogueLoads()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const QList<CollageTemplate> entries = catalogue.catalogue();
    QCOMPARE(entries.size(), 55);

    const QStringList families = {QStringLiteral("full"),   QStringLiteral("equal"),
                                  QStringLiteral("hero"),   QStringLiteral("strip"),
                                  QStringLiteral("mosaic"), QStringLiteral("frame"),
                                  QStringLiteral("bands"),  QStringLiteral("columns")};
    QSet<QString> ids;
    int perCount[11] = {};
    for (const CollageTemplate &t : entries) {
        QVERIFY(!t.id.isEmpty());
        QVERIFY(!ids.contains(t.id));
        ids.insert(t.id);
        QVERIFY(!t.name.isEmpty());
        QVERIFY(!t.nameEn.isEmpty());
        QVERIFY(families.contains(t.family));
        QVERIFY(t.photoCount >= 1 && t.photoCount <= 10);
        QCOMPARE(t.slotList.size(), t.photoCount);
        perCount[t.photoCount] += 1;

        // The slots are a partition of the unit square on the 1/1000 lattice.
        long long area = 0;
        for (const CollageSlot &slot : t.slotList) {
            const QRectF &rect = slot.rect;
            QVERIFY(rect.width() > 0.0 && rect.height() > 0.0);
            QVERIFY(rect.left() >= 0.0 && rect.top() >= 0.0);
            QVERIFY(rect.right() <= 1.0 + 1e-9 && rect.bottom() <= 1.0 + 1e-9);
            for (double value : {rect.left(), rect.top(), rect.width(), rect.height()}) {
                const double scaled = value * 1000.0;
                QVERIFY(std::abs(scaled - std::round(scaled)) < 1e-6);
            }
            area += 1LL * std::lround(rect.width() * 1000.0)
                * std::lround(rect.height() * 1000.0);
        }
        QCOMPARE(area, 1000000LL);
        for (int i = 0; i < t.slotList.size(); ++i) {
            for (int j = i + 1; j < t.slotList.size(); ++j) {
                const QRectF overlap = t.slotList.at(i).rect.intersected(t.slotList.at(j).rect);
                QVERIFY(overlap.width() * overlap.height() < 1e-12);
            }
        }
    }
    QCOMPARE(perCount[1], 1);
    for (int count = 2; count <= 10; ++count) {
        QCOMPARE(perCount[count], 6);
        QCOMPARE(catalogue.templatesForCount(count).size(), 6);
        for (const CollageTemplate &t : catalogue.templatesForCount(count))
            QCOMPARE(t.photoCount, count);
    }
    QCOMPARE(catalogue.templatesForCount(1).size(), 1);
    QVERIFY(catalogue.templatesForCount(0).isEmpty());
    QVERIFY(catalogue.templatesForCount(11).isEmpty());

    QVERIFY(catalogue.find(QStringLiteral("n06-two-four")) != nullptr);
    QVERIFY(catalogue.find(QStringLiteral("n00-missing")) == nullptr);

    // The strict parser accepts the shipped file and produces the same order.
    const auto parsed = CollageTemplates::fromJson(shippedJson());
    if (!parsed.ok())
        QFAIL(qPrintable(parsed.error().message));
    QCOMPARE(parsed.value().catalogue().size(), entries.size());
    for (int i = 0; i < entries.size(); ++i)
        QCOMPARE(parsed.value().catalogue().at(i).id, entries.at(i).id);
}

void TestCollageTemplates::catalogueOrderIsFillOrder()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();

    // Slot zero is the feature slot, not the top-left cell: on this layout the
    // hero is the large top-left rectangle and the first satellite is a thin
    // band with a centre above the hero's centre. A sort by centre would put
    // the photograph in the wrong slot.
    const CollageTemplate *corner = catalogue.find(QStringLiteral("n06-hero-corner-5"));
    QVERIFY(corner);
    QCOMPARE(corner->slotList.size(), 6);
    QVERIFY(corner->slotList.at(0).rect.width() * corner->slotList.at(0).rect.height()
            > corner->slotList.at(1).rect.width() * corner->slotList.at(1).rect.height());
    QVERIFY(corner->slotList.at(0).rect.center().y() > corner->slotList.at(1).rect.center().y());

    // The two-photo splits are splits, so slot zero is the left or the top one.
    const CollageTemplate *side = catalogue.find(QStringLiteral("n02-hero-left"));
    QVERIFY(side);
    QCOMPARE(side->slotList.at(0).rect.left(), 0.0);
    QCOMPARE(side->slotList.at(0).rect.width(), 0.6);
    const CollageTemplate *top = catalogue.find(QStringLiteral("n02-hero-top"));
    QVERIFY(top);
    QCOMPARE(top->slotList.at(0).rect.top(), 0.0);
    QCOMPARE(top->slotList.at(0).rect.height(), 0.6);

    // The single-photo fallback reproduces the full content box.
    const CollageTemplate *full = catalogue.find(QStringLiteral("n01-full"));
    QVERIFY(full);
    QCOMPARE(full->photoCount, 1);
    QCOMPARE(full->slotList.size(), 1);
    QCOMPARE(full->slotList.at(0).rect, QRectF(0.0, 0.0, 1.0, 1.0));
}

void TestCollageTemplates::rejectsMalformedCatalogue()
{
    auto rejects = [](const QByteArray &json, const QString &expected) {
        const auto parsed = CollageTemplates::fromJson(json);
        if (parsed.ok()) {
            QFAIL("a malformed catalogue was accepted");
            return;
        }
        QCOMPARE(parsed.error().code, expected);
        QVERIFY(!parsed.error().message.isEmpty());
    };

    const QJsonObject root = shippedRoot();
    QVERIFY(!root.isEmpty());
    QCOMPARE(templatesOf(root).size(), 55);

    rejects(QByteArrayLiteral("{"), kInvalid);
    rejects(QByteArrayLiteral("[]"), kInvalid);
    rejects(QByteArrayLiteral("{}"), kInvalid);

    QJsonObject wrongFormat = root;
    wrongFormat.insert(QStringLiteral("format"), QStringLiteral("photoprint.collage/2"));
    rejects(encoded(wrongFormat), kInvalid);

    QJsonObject wrongLattice = root;
    wrongLattice.insert(QStringLiteral("lattice"), 100);
    rejects(encoded(wrongLattice), kInvalid);

    QJsonObject unknownRoot = root;
    unknownRoot.insert(QStringLiteral("version"), 1);
    rejects(encoded(unknownRoot), kInvalid);

    QJsonObject unitNotString = root;
    unitNotString.insert(QStringLiteral("unit"), 5);
    rejects(encoded(unitNotString), kInvalid);

    QJsonObject templatesNotArray = root;
    templatesNotArray.insert(QStringLiteral("templates"), 5);
    rejects(encoded(templatesNotArray), kInvalid);

    // 54 entries: the catalogue must hold the full set.
    QJsonArray entries = templatesOf(root);
    entries.removeAt(0);
    rejects(encoded(withTemplates(root, entries)), kInvalid);

    // 55 entries, but the single-photo fallback became a two-photo layout.
    QJsonArray counts = templatesOf(root);
    QJsonObject fallback = counts.at(0).toObject();
    fallback.insert(QStringLiteral("photoCount"), 2);
    fallback.insert(QStringLiteral("slots"),
                    QJsonArray{
                        QJsonObject{{QStringLiteral("rect"), QJsonArray{0.0, 0.0, 0.5, 1.0}},
                                    {QStringLiteral("aspect"), QStringLiteral("portrait")}},
                        QJsonObject{{QStringLiteral("rect"), QJsonArray{0.5, 0.0, 0.5, 1.0}},
                                    {QStringLiteral("aspect"), QStringLiteral("portrait")}}});
    counts.replace(0, fallback);
    rejects(encoded(withTemplates(root, counts)), kInvalid);

    // A duplicated id.
    QJsonArray duplicateIds = templatesOf(root);
    QJsonObject duplicate = duplicateIds.at(1).toObject();
    duplicate.insert(QStringLiteral("id"), templatesOf(root).at(0).toObject()
                                              .value(QStringLiteral("id")));
    duplicateIds.replace(1, duplicate);
    rejects(encoded(withTemplates(root, duplicateIds)), kInvalid);

    // Missing and empty names.
    QJsonObject noName = templateAt(root, 1);
    noName.remove(QStringLiteral("name"));
    rejects(encoded(replaceTemplate(root, 1, noName)), kInvalid);
    QJsonObject emptyName = templateAt(root, 1);
    emptyName.insert(QStringLiteral("name"), QString());
    rejects(encoded(replaceTemplate(root, 1, emptyName)), kInvalid);

    QJsonObject unknownFamily = templateAt(root, 1);
    unknownFamily.insert(QStringLiteral("family"), QStringLiteral("grid"));
    rejects(encoded(replaceTemplate(root, 1, unknownFamily)), kInvalid);

    QJsonObject unknownTemplateField = templateAt(root, 1);
    unknownTemplateField.insert(QStringLiteral("columns"), 2);
    rejects(encoded(replaceTemplate(root, 1, unknownTemplateField)), kInvalid);

    // The per-orientation override is not silently dropped.
    QJsonObject landscapeSlots = templateAt(root, 1);
    landscapeSlots.insert(QStringLiteral("landscapeSlots"),
                          landscapeSlots.value(QStringLiteral("slots")));
    rejects(encoded(replaceTemplate(root, 1, landscapeSlots)), kInvalid);

    QJsonObject fractionalCount = templateAt(root, 1);
    fractionalCount.insert(QStringLiteral("photoCount"), 2.5);
    rejects(encoded(replaceTemplate(root, 1, fractionalCount)), kInvalid);

    QJsonObject countMismatch = templateAt(root, 1);
    countMismatch.insert(QStringLiteral("photoCount"), 3);
    rejects(encoded(replaceTemplate(root, 1, countMismatch)), kInvalid);

    QJsonObject outOfRangeCount = templateAt(root, 1);
    outOfRangeCount.insert(QStringLiteral("photoCount"), 11);
    rejects(encoded(replaceTemplate(root, 1, outOfRangeCount)), kInvalid);

    QJsonObject slotNotObject = templateAt(root, 1);
    QJsonArray slotArray = slotNotObject.value(QStringLiteral("slots")).toArray();
    slotArray.replace(0, 5);
    slotNotObject.insert(QStringLiteral("slots"), slotArray);
    rejects(encoded(replaceTemplate(root, 1, slotNotObject)), kInvalid);

    QJsonObject unknownSlotField = templateAt(root, 1).value(QStringLiteral("slots")).toArray()
                                       .at(0).toObject();
    unknownSlotField.insert(QStringLiteral("weight"), 1);
    rejects(encoded(replaceSlot(root, 1, 0, unknownSlotField)), kInvalid);

    QJsonObject shortRect = templateAt(root, 1).value(QStringLiteral("slots")).toArray()
                                .at(0).toObject();
    shortRect.insert(QStringLiteral("rect"), QJsonArray{0.0, 0.0, 0.5});
    rejects(encoded(replaceSlot(root, 1, 0, shortRect)), kInvalid);

    // 0.3333 is not on the 1/1000 lattice.
    QJsonObject offLattice = templateAt(root, 1).value(QStringLiteral("slots")).toArray()
                                 .at(0).toObject();
    rejects(encoded(replaceSlot(root, 1, 0, withRect(offLattice, 0, 0.3333))), kInvalid);

    // Zero width, and a rectangle that leaves the unit square.
    QJsonObject zeroWidth = withRect(offLattice, 2, 0.0);
    rejects(encoded(replaceSlot(root, 1, 0, zeroWidth)), kInvalid);
    QJsonObject outside = withRect(withRect(offLattice, 0, 0.6), 2, 0.5);
    rejects(encoded(replaceSlot(root, 1, 0, outside)), kInvalid);

    // Two rectangles of area one that overlap instead of tiling.
    QJsonObject overlapping = templateAt(root, 1);
    overlapping.insert(QStringLiteral("slots"),
                       QJsonArray{
                           QJsonObject{{QStringLiteral("rect"), QJsonArray{0.0, 0.0, 0.6, 1.0}},
                                       {QStringLiteral("aspect"), QStringLiteral("portrait")}},
                           QJsonObject{{QStringLiteral("rect"), QJsonArray{0.5, 0.0, 0.4, 1.0}},
                                       {QStringLiteral("aspect"), QStringLiteral("portrait")}}});
    rejects(encoded(replaceTemplate(root, 1, overlapping)), kInvalid);

    QJsonObject unknownAspect = templateAt(root, 1).value(QStringLiteral("slots")).toArray()
                                    .at(0).toObject();
    unknownAspect.insert(QStringLiteral("aspect"), QStringLiteral("wide"));
    rejects(encoded(replaceSlot(root, 1, 0, unknownAspect)), kInvalid);

    // The hint must agree with the rectangle: this slot is portrait.
    QJsonObject wrongAspect = templateAt(root, 1).value(QStringLiteral("slots")).toArray()
                                  .at(0).toObject();
    wrongAspect.insert(QStringLiteral("aspect"), QStringLiteral("square"));
    rejects(encoded(replaceSlot(root, 1, 0, wrongAspect)), kInvalid);
}

void TestCollageTemplates::mapsEveryPaperBothWays()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const QList<PaperSpec> papers = defaultPapers();
    QCOMPARE(papers.size(), 6);

    for (const PaperSpec &paper : papers) {
        for (bool landscape : {false, true}) {
            const QSizeF sheet = PaperGeometry::forPaper(paper, landscape).physicalSizeMm();
            const double margin = collageMarginMm(sheet);
            QVERIFY(margin >= 4.0);
            QVERIFY(margin <= 10.0);
            QCOMPARE(margin, std::max(4.0, std::min(10.0, 0.05 * std::min(sheet.width(),
                                                                        sheet.height()))));

            for (const CollageTemplate &t : catalogue.catalogue()) {
                const auto mapped = catalogue.slotsMm(t, paper, landscape);
                if (!mapped.ok()) {
                    QFAIL(qPrintable(QStringLiteral("%1 on %2: %3")
                                         .arg(t.id, paper.name, mapped.error().message)));
                }
                const QList<QRectF> rects = mapped.value();
                QCOMPARE(rects.size(), t.photoCount);

                double left = 1e9;
                double top = 1e9;
                double right = -1e9;
                double bottom = -1e9;
                for (const QRectF &rect : rects) {
                    QVERIFY(rect.width() >= 15.0);
                    QVERIFY(rect.height() >= 15.0);
                    QVERIFY(rect.left() >= -1e-9);
                    QVERIFY(rect.top() >= -1e-9);
                    QVERIFY(rect.right() <= sheet.width() + 1e-9);
                    QVERIFY(rect.bottom() <= sheet.height() + 1e-9);
                    left = std::min(left, rect.left());
                    top = std::min(top, rect.top());
                    right = std::max(right, rect.right());
                    bottom = std::max(bottom, rect.bottom());
                }
                // One margin on all four sides, because the slots tile the
                // content box and every edge gives up half a gutter.
                QVERIFY(near(left, margin));
                QVERIFY(near(top, margin));
                QVERIFY(near(right, sheet.width() - margin));
                QVERIFY(near(bottom, sheet.height() - margin));

                // Every pair of edge-sharing neighbours is one gutter apart.
                for (int i = 0; i < t.slotList.size(); ++i) {
                    for (int j = 0; j < t.slotList.size(); ++j) {
                        if (i == j)
                            continue;
                        const QRectF &a = t.slotList.at(i).rect;
                        const QRectF &b = t.slotList.at(j).rect;
                        const double sharedWidth =
                            std::min(a.right(), b.right()) - std::max(a.left(), b.left());
                        const double sharedHeight =
                            std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
                        if (sharedWidth > 1e-9 && near(a.bottom(), b.top()))
                            QVERIFY(near(rects.at(j).top() - rects.at(i).bottom(), margin));
                        if (sharedHeight > 1e-9 && near(a.right(), b.left()))
                            QVERIFY(near(rects.at(j).left() - rects.at(i).right(), margin));
                    }
                }
            }
        }
    }
}

void TestCollageTemplates::usesPhysicalSheetAndIgnoresImageable()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *equalSide = catalogue.find(QStringLiteral("n02-equal-side"));
    QVERIFY(equalSide);

    // 10 × 15 cm is sold as 100 × 150 mm, but the driver wants 4 × 6 inches.
    const PaperSpec photo = paperNamed(QStringLiteral("10 × 15 cm"));
    QCOMPARE(photo.physicalWidthMm, 100.0);
    QCOMPARE(photo.physicalHeightMm, 150.0);
    QCOMPARE(photo.logicalWidthMm, 101.6);
    QCOMPARE(photo.logicalHeightMm, 152.4);

    PaperGeometry geometry = PaperGeometry::forPaper(photo, false);
    QVERIFY(!geometry.imageableKnown);
    QCOMPARE(geometry.physicalSizeMm(), QSizeF(100.0, 150.0));
    QCOMPARE(geometry.logicalSizeMm(), QSizeF(101.6, 152.4));

    const auto mapped = catalogue.slotsMm(*equalSide, geometry);
    QVERIFY(mapped.ok());
    QCOMPARE(mapped.value().size(), 2);
    // Margin 5 mm, content box 95 × 145 mm, so each half is 42.5 × 140 mm.
    QVERIFY(near(mapped.value().at(0).width(), 42.5));
    QVERIFY(near(mapped.value().at(0).height(), 140.0));
    QVERIFY(near(mapped.value().at(0).left(), 5.0));
    QVERIFY(near(mapped.value().at(1).left(), 52.5));
    QVERIFY(near(mapped.value().at(1).right(), 95.0));

    // The mapper never reads the imageable rectangle: an unknown one, and a
    // wildly wrong known one, produce the same geometry.
    PaperGeometry clipped = geometry;
    clipped.imageableKnown = true;
    clipped.imageableMm = QRectF(4.0, 4.0, 92.0, 142.0);
    const auto clippedSlots = catalogue.slotsMm(*equalSide, clipped);
    QVERIFY(clippedSlots.ok());
    QCOMPARE(clippedSlots.value().size(), mapped.value().size());
    for (int i = 0; i < mapped.value().size(); ++i) {
        QCOMPARE(clippedSlots.value().at(i).left(), mapped.value().at(i).left());
        QCOMPARE(clippedSlots.value().at(i).top(), mapped.value().at(i).top());
        QCOMPARE(clippedSlots.value().at(i).width(), mapped.value().at(i).width());
        QCOMPARE(clippedSlots.value().at(i).height(), mapped.value().at(i).height());
    }

    // Landscape turns the same normalised slots through the content box.
    const auto turned = catalogue.slotsMm(*equalSide, photo, true);
    QVERIFY(turned.ok());
    QVERIFY(near(turned.value().at(0).width(), 67.5));
    QVERIFY(near(turned.value().at(0).height(), 90.0));

    // The four-narrow strip is the tightest layout, and 9 × 13 cm the tightest
    // paper: 0.25 × (88.9 − 4.445) − 4.445 = 16.66875 mm.
    const CollageTemplate *strip = catalogue.find(QStringLiteral("n04-strip-4"));
    QVERIFY(strip);
    const PaperSpec small = paperNamed(QStringLiteral("9 × 13 cm"));
    const auto stripSlots = catalogue.slotsMm(*strip, small, false);
    QVERIFY(stripSlots.ok());
    QVERIFY(near(stripSlots.value().at(0).width(), 16.66875));
    QVERIFY(near(stripSlots.value().at(0).height(), 118.11));
    QVERIFY(near(collageMarginMm(QSizeF(88.9, 127.0)), 4.445));
}

void TestCollageTemplates::refusesSmallPaper()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    PaperSpec custom;
    custom.name = QStringLiteral("60 × 80");
    custom.physicalWidthMm = 60.0;
    custom.physicalHeightMm = 80.0;
    custom.type = PaperType::Glossy;
    custom.custom = true;

    const CollageTemplate *strip = catalogue.find(QStringLiteral("n04-strip-4"));
    QVERIFY(strip);
    const auto mapped = catalogue.slotsMm(*strip, custom, false);
    QVERIFY(!mapped.ok());
    QCOMPARE(mapped.error().code, kTooSmall);
    QVERIFY(mapped.error().message.contains(QStringLiteral("60 × 80")));
    QVERIFY(mapped.error().message.contains(QStringLiteral("túl kicsi")));

    // Applying it on that paper is refused for the same reason, before any
    // frame is touched.
    const auto applied = catalogue.apply(*strip, custom, false, framesWith(*strip));
    QVERIFY(!applied.ok());
    QCOMPARE(applied.error().code, kTooSmall);
    QVERIFY(applied.error().message.contains(QStringLiteral("60 × 80")));

    // The full-page fallback is the one template a small sheet can still hold:
    // 60 − 2 × 4 = 52 mm and 80 − 2 × 4 = 72 mm.
    const CollageTemplate *full = catalogue.find(QStringLiteral("n01-full"));
    QVERIFY(full);
    const auto fullSlots = catalogue.slotsMm(*full, custom, false);
    QVERIFY(fullSlots.ok());
    QCOMPARE(fullSlots.value().size(), 1);
    QVERIFY(near(fullSlots.value().at(0).width(), 52.0));
    QVERIFY(near(fullSlots.value().at(0).height(), 72.0));

    // A degenerate sheet names itself by size and is refused, never divided by.
    PaperSpec empty;
    empty.physicalWidthMm = 0.0;
    empty.physicalHeightMm = 0.0;
    const auto degenerate = catalogue.slotsMm(*strip, empty, false);
    QVERIFY(!degenerate.ok());
    QCOMPARE(degenerate.error().code, kTooSmall);
    QVERIFY(!degenerate.error().message.isEmpty());
}

void TestCollageTemplates::applyPreservesFrameFields()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const PaperSpec photo = paperNamed(QStringLiteral("10 × 15 cm"));
    int applied = 0;

    for (const CollageTemplate &t : catalogue.catalogue()) {
        QVERIFY(t.photoCount >= 1 && t.photoCount <= 10);
        const auto mapped = catalogue.slotsMm(t, photo, false);
        QVERIFY(mapped.ok());
        QList<Frame> frames = framesWith(t);
        // Match the slot aspect so the crop policy keeps the crop; the reset
        // path has its own test.
        for (int i = 0; i < frames.size(); ++i) {
            frames[i].width = mapped.value().at(i).width();
            frames[i].height = mapped.value().at(i).height();
        }

        const auto result = catalogue.apply(t, photo, false, frames);
        if (!result.ok())
            QFAIL(qPrintable(result.error().message));
        QCOMPARE(result.value().size(), frames.size());

        for (int i = 0; i < frames.size(); ++i) {
            const Frame &before = frames.at(i);
            const Frame &after = result.value().at(i);
            QCOMPARE(after.id, before.id);
            QCOMPARE(after.sourcePath, before.sourcePath);
            QCOMPARE(after.sourceSha256, before.sourceSha256);
            QCOMPARE(after.imageRotationDeg, before.imageRotationDeg);
            QCOMPARE(after.label, before.label);
            QCOMPARE(after.paperOverride, before.paperOverride);
            QCOMPARE(after.paperEv, before.paperEv);
            QVERIFY(after.tone == before.tone);
            QCOMPARE(after.autoTone, before.autoTone);
            QCOMPARE(after.brightness, before.brightness);
            QCOMPARE(after.filter, before.filter);
            QCOMPARE(after.effectVersion, before.effectVersion);
            QCOMPARE(after.filterStrength, before.filterStrength);
            QCOMPARE(after.upscale, before.upscale);

            QVERIFY(near(after.cropCenter.x(), before.cropCenter.x()));
            QVERIFY(near(after.cropCenter.y(), before.cropCenter.y()));
            QVERIFY(near(after.zoom, before.zoom));

            QVERIFY(near(after.cx, mapped.value().at(i).center().x()));
            QVERIFY(near(after.cy, mapped.value().at(i).center().y()));
            QVERIFY(near(after.width, mapped.value().at(i).width()));
            QVERIFY(near(after.height, mapped.value().at(i).height()));
            QCOMPARE(after.angleDeg, 0.0);
            QVERIFY(after.fit == FitMode::Fill);
            QVERIFY(!after.autoPlaced);
        }
        applied += 1;
    }
    QCOMPARE(applied, 55);
}

void TestCollageTemplates::applyCropPolicy()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const PaperSpec photo = paperNamed(QStringLiteral("10 × 15 cm"));
    const CollageTemplate *equalSide = catalogue.find(QStringLiteral("n02-equal-side"));
    QVERIFY(equalSide);
    const auto mapped = catalogue.slotsMm(*equalSide, photo, false);
    QVERIFY(mapped.ok());

    auto appliedWith = [&](double aspectFactor) {
        QList<Frame> frames = framesWith(*equalSide);
        for (int i = 0; i < frames.size(); ++i) {
            frames[i].width = mapped.value().at(i).width();
            frames[i].height = mapped.value().at(i).height() / aspectFactor;
        }
        return catalogue.apply(*equalSide, photo, false, frames);
    };

    // Within 15 %: the user's crop and zoom survive.
    const auto kept = appliedWith(1.1);
    QVERIFY(kept.ok());
    for (const Frame &frame : kept.value()) {
        QVERIFY(near(frame.cropCenter.x(), 0.3));
        QVERIFY(near(frame.cropCenter.y(), 0.4));
        QVERIFY(near(frame.zoom, 1.25));
    }

    // Outside 15 %: the crop resets to the centre and zoom to 1.
    const auto reset = appliedWith(1.5);
    QVERIFY(reset.ok());
    for (const Frame &frame : reset.value()) {
        QVERIFY(near(frame.cropCenter.x(), 0.5));
        QVERIFY(near(frame.cropCenter.y(), 0.5));
        QCOMPARE(frame.zoom, 1.0);
    }
}

void TestCollageTemplates::applyThroughPage()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n05-three-two"));
    QVERIFY(t);
    Page page;
    page.paper = paperNamed(QStringLiteral("A5"));
    page.landscape = true;
    page.frames = framesWith(*t);

    const auto fromPage = catalogue.apply(*t, page);
    QVERIFY(fromPage.ok());
    const auto fromParts = catalogue.apply(*t, page.paper, page.landscape, page.frames);
    QVERIFY(fromParts.ok());
    QCOMPARE(fromPage.value().size(), fromParts.value().size());
    for (int i = 0; i < fromPage.value().size(); ++i)
        QVERIFY(fromPage.value().at(i) == fromParts.value().at(i));
    QCOMPARE(fromPage.value().size(), 5);
}

void TestCollageTemplates::applyRefusesWrongCount()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n02-equal-side"));
    QVERIFY(t);

    QList<Frame> three;
    for (int i = 0; i < 3; ++i)
        three.append(frameWith(i, QStringLiteral("wrong")));
    const auto wrong = catalogue.apply(*t, paperNamed(QStringLiteral("A4")), false, three);
    QVERIFY(!wrong.ok());
    QCOMPARE(wrong.error().code, QStringLiteral("template-count-mismatch"));
    QVERIFY(wrong.error().message.contains(QStringLiteral("2")));
    QVERIFY(wrong.error().message.contains(QStringLiteral("3")));

    QList<Frame> one;
    one.append(frameWith(0, QStringLiteral("wrong")));
    const auto tooFew = catalogue.apply(*t, paperNamed(QStringLiteral("A4")), false, one);
    QVERIFY(!tooFew.ok());
    QCOMPARE(tooFew.error().code, QStringLiteral("template-count-mismatch"));
}

void TestCollageTemplates::applyRefusesMissingSource()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n02-equal-side"));
    QVERIFY(t);

    QList<Frame> frames = framesWith(*t);
    frames[1].sourcePath.clear();
    const auto result = catalogue.apply(*t, paperNamed(QStringLiteral("A4")), false, frames);
    QVERIFY(!result.ok());
    QCOMPARE(result.error().code, QStringLiteral("template-missing-source"));
    QVERIFY(!result.error().message.isEmpty());
}

void TestCollageTemplates::addedPhotoDoesNotReflow()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *t = catalogue.find(QStringLiteral("n06-grid-3x2"));
    QVERIFY(t);
    Page page;
    page.paper = paperNamed(QStringLiteral("10 × 15 cm"));

    const auto applied = catalogue.apply(*t, page.paper, page.landscape, framesWith(*t));
    QVERIFY(applied.ok());
    page.frames = applied.value();

    Frame extra = frameWith(6, QStringLiteral("added"));
    extra.sourcePath = QStringLiteral("/photos/added.jpg");
    const QList<Frame> afterAppend = LayoutEngine::appendPhoto(page, page.frames, extra, {});
    QCOMPARE(afterAppend.size(), 7);
    for (int i = 0; i < applied.value().size(); ++i)
        QVERIFY(afterAppend.at(i) == applied.value().at(i));
    QCOMPARE(afterAppend.last().id, extra.id);

    // Control: the same six frames with autoPlaced left alone do reflow, which
    // is what makes the assertion above meaningful.
    QList<Frame> autoFrames = applied.value();
    for (Frame &frame : autoFrames)
        frame.autoPlaced = true;
    const QList<Frame> regridded = LayoutEngine::appendPhoto(page, autoFrames, extra, {});
    QCOMPARE(regridded.size(), 7);
    QVERIFY(!near(regridded.at(0).width, applied.value().at(0).width));
}

void TestCollageTemplates::labelsAndReferenceNames()
{
    const CollageTemplates &catalogue = CollageTemplates::instance();
    const CollageTemplate *hero = catalogue.find(QStringLiteral("n02-hero-left"));
    QVERIFY(hero);
    QCOMPARE(collageLabel(*hero), QStringLiteral("hero · 2"));
    QCOMPARE(collageReferenceName(*hero), hero->name);
    QCOMPARE(collageReferenceName(*hero), QStringLiteral("Nagy kép balra, keskeny jobbra"));

    const CollageTemplate *columns = catalogue.find(QStringLiteral("n09-cols-4-3-2"));
    QVERIFY(columns);
    QCOMPARE(collageLabel(*columns), QStringLiteral("columns · 9"));

    // A template built by hand without a name still has a usable reference.
    CollageTemplate anonymous;
    anonymous.id = QStringLiteral("n02-custom");
    anonymous.photoCount = 2;
    QCOMPARE(collageReferenceName(anonymous), anonymous.id);
}

QTEST_GUILESS_MAIN(TestCollageTemplates)
#include "tst_collage_templates.moc"
