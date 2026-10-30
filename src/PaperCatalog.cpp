#include "PaperCatalog.h"

#include <QCoreApplication>
#include <QSettings>

#include <cmath>

namespace pp {

namespace {

constexpr double kSizeTolerance = 0.45;

bool sameSize(double width, double height, double otherWidth, double otherHeight)
{
    const auto close = [](double a, double b) { return std::abs(a - b) <= kSizeTolerance; };
    return (close(width, otherWidth) && close(height, otherHeight))
        || (close(width, otherHeight) && close(height, otherWidth));
}

/// Centimetre text without trailing zeros: 210 -> "21", 297 -> "29,7".
QString cm(double millimetres)
{
    QString text = QString::number(millimetres / 10.0, 'f', 2);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    text.replace(QLatin1Char('.'), QLatin1Char(','));
    return text;
}

PaperSpec photoPaper(const QString &name, double widthMm, double heightMm, double logicalWidthMm,
                     double logicalHeightMm)
{
    PaperSpec paper;
    paper.name = name;
    paper.physicalWidthMm = widthMm;
    paper.physicalHeightMm = heightMm;
    paper.logicalWidthMm = logicalWidthMm;
    paper.logicalHeightMm = logicalHeightMm;
    paper.type = PaperType::Glossy;
    paper.borderlessSupported = true;
    return paper;
}

void writeAll(QSettings &settings, const QList<PaperSpec> &papers)
{
    settings.remove(QStringLiteral("customPapers"));
    settings.beginWriteArray(QStringLiteral("customPapers"), papers.size());
    for (int i = 0; i < papers.size(); ++i) {
        settings.setArrayIndex(i);
        const PaperSpec &paper = papers.at(i);
        settings.setValue(QStringLiteral("name"), paper.name);
        settings.setValue(QStringLiteral("widthMm"), paper.physicalWidthMm);
        settings.setValue(QStringLiteral("heightMm"), paper.physicalHeightMm);
        settings.setValue(QStringLiteral("logicalWidthMm"), paper.logicalWidthMm);
        settings.setValue(QStringLiteral("logicalHeightMm"), paper.logicalHeightMm);
        settings.setValue(QStringLiteral("type"), paperTypeKey(paper.type));
        settings.setValue(QStringLiteral("borderless"), paper.borderlessSupported);
        settings.setValue(QStringLiteral("hasCheckedOffset"), paper.hasCheckedOffset);
        settings.setValue(QStringLiteral("offsetXMm"), paper.offsetXMm);
        settings.setValue(QStringLiteral("offsetYMm"), paper.offsetYMm);
    }
    settings.endArray();
    settings.sync();
}

} // namespace

QList<PaperSpec> defaultPapers()
{
    PaperSpec a4;
    a4.name = QStringLiteral("A4");
    a4.physicalWidthMm = 210.0;
    a4.physicalHeightMm = 297.0;

    PaperSpec a5;
    a5.name = QStringLiteral("A5");
    a5.physicalWidthMm = 148.0;
    a5.physicalHeightMm = 210.0;

    PaperSpec a6;
    a6.name = QStringLiteral("A6");
    a6.physicalWidthMm = 105.0;
    a6.physicalHeightMm = 148.0;

    // European photo paper is sold by the centimetre name; the printer's
    // logical media is the nearest inch-based mode. The physical size is the
    // paper actually loaded, the logical one is what the driver expects.
    return {a4, a5, a6,
            photoPaper(QStringLiteral("9 × 13 cm"), 88.9, 127.0, 88.9, 127.0),
            photoPaper(QStringLiteral("10 × 15 cm"), 100.0, 150.0, 101.6, 152.4),
            photoPaper(QStringLiteral("13 × 18 cm"), 127.0, 177.8, 127.0, 177.8)};
}

QString paperLabel(const PaperSpec &paper)
{
    if (paper.name == QLatin1String("A4"))
        return QStringLiteral("A4 (21 × 29,7 cm)");
    if (paper.name == QLatin1String("A5"))
        return QStringLiteral("A5 (14,8 × 21 cm)");
    if (paper.name == QLatin1String("A6"))
        return QStringLiteral("A6 (10,5 × 14,8 cm)");
    if (paper.name.isEmpty())
        return sizeLabelCm(paper.physicalWidthMm, paper.physicalHeightMm);
    return paper.name;
}

QString sizeLabelCm(double widthMm, double heightMm)
{
    return QStringLiteral("%1 × %2 cm").arg(cm(widthMm), cm(heightMm));
}

QString paperTypeLabel(PaperType type)
{
    switch (type) {
    case PaperType::Glossy:
        return QCoreApplication::translate("PhotoPrint", "Fényes");
    case PaperType::Matte:
        return QCoreApplication::translate("PhotoPrint", "Matt");
    case PaperType::Plain:
        break;
    }
    return QCoreApplication::translate("PhotoPrint", "Normál");
}

QString paperTypeKey(PaperType type)
{
    switch (type) {
    case PaperType::Glossy:
        return QStringLiteral("glossy");
    case PaperType::Matte:
        return QStringLiteral("matte");
    case PaperType::Plain:
        break;
    }
    return QStringLiteral("plain");
}

PaperType paperTypeFromKey(const QString &key, bool legacyPhoto)
{
    if (key == QLatin1String("glossy"))
        return PaperType::Glossy;
    if (key == QLatin1String("matte"))
        return PaperType::Matte;
    if (key == QLatin1String("plain"))
        return PaperType::Plain;
    return legacyPhoto ? PaperType::Glossy : PaperType::Plain;
}

bool isFixedPhotoSize(const PaperSpec &paper)
{
    return sameSize(paper.physicalWidthMm, paper.physicalHeightMm, 88.9, 127.0)
        || sameSize(paper.physicalWidthMm, paper.physicalHeightMm, 100.0, 150.0)
        || sameSize(paper.physicalWidthMm, paper.physicalHeightMm, 127.0, 177.8);
}

CustomPaperStore::CustomPaperStore(const QString &settingsFile)
    : m_settings(settingsFile.isEmpty()
                     ? std::make_unique<QSettings>()
                     : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat))
{
}

CustomPaperStore::~CustomPaperStore() = default;

Result<QList<PaperSpec>> CustomPaperStore::readAll() const
{
    QList<PaperSpec> papers;
    const int count = m_settings->beginReadArray(QStringLiteral("customPapers"));
    for (int i = 0; i < count; ++i) {
        m_settings->setArrayIndex(i);
        PaperSpec paper;
        paper.name = m_settings->value(QStringLiteral("name")).toString().trimmed();
        paper.physicalWidthMm = m_settings->value(QStringLiteral("widthMm")).toDouble();
        paper.physicalHeightMm = m_settings->value(QStringLiteral("heightMm")).toDouble();
        // A missing logical size means "same as the physical size".
        paper.logicalWidthMm = m_settings->value(QStringLiteral("logicalWidthMm")).toDouble();
        paper.logicalHeightMm = m_settings->value(QStringLiteral("logicalHeightMm")).toDouble();
        paper.type = paperTypeFromKey(m_settings->value(QStringLiteral("type")).toString());
        paper.borderlessSupported = m_settings->value(QStringLiteral("borderless")).toBool();
        paper.hasCheckedOffset = m_settings->value(QStringLiteral("hasCheckedOffset")).toBool();
        paper.offsetXMm = m_settings->value(QStringLiteral("offsetXMm")).toDouble();
        paper.offsetYMm = m_settings->value(QStringLiteral("offsetYMm")).toDouble();
        // Everything in this store was defined by the user: the flag follows
        // from the store, it is not a second piece of information to keep.
        paper.custom = true;
        if (paper.name.isEmpty() || !(paper.physicalWidthMm > 0.0)
            || !(paper.physicalHeightMm > 0.0))
            continue;
        papers.append(paper);
    }
    m_settings->endArray();

    // A list rebuilt from a half-read file would be written straight back by the
    // next save(), and every paper the file still held would be gone. Report the
    // damage instead of pretending the user has no papers.
    switch (m_settings->status()) {
    case QSettings::NoError:
        break;
    case QSettings::FormatError:
        return error(QStringLiteral("paper-corrupt"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "A papírbeállítások fájlja sérült."));
    case QSettings::AccessError:
        return error(QStringLiteral("paper-read"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "A papírbeállítások nem olvashatók."));
    }

    // QSettings reads an unparsable array count as "no entries" without any
    // error, which looks exactly like an empty store. The count key is written
    // by every save, so entries without it can only come from a damaged file.
    if (count == 0) {
        const QString prefix = QStringLiteral("customPapers/");
        for (const QString &key : m_settings->allKeys()) {
            if (key.startsWith(prefix) && key != prefix + QStringLiteral("size")) {
                return error(QStringLiteral("paper-corrupt"),
                             QCoreApplication::translate("PhotoPrint",
                                                         "A papírbeállítások fájlja sérült."));
            }
        }
    }
    return papers;
}

QList<PaperSpec> CustomPaperStore::all() const
{
    const auto read = readAll();
    return read ? read.value() : QList<PaperSpec>{};
}

Result<void> CustomPaperStore::save(const PaperSpec &paper)
{
    PaperSpec stored = paper;
    stored.name = stored.name.trimmed();
    if (stored.name.isEmpty())
        return error(QStringLiteral("paper-name"), QCoreApplication::translate("PhotoPrint", "Adj nevet a papírnak."));
    if (!(stored.physicalWidthMm > 0.0) || !(stored.physicalHeightMm > 0.0))
        return error(QStringLiteral("paper-size"), QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));

    const auto existing = readAll();
    if (!existing)
        return existing.error();
    QList<PaperSpec> papers = existing.value();
    for (int i = papers.size() - 1; i >= 0; --i) {
        if (papers.at(i).name.compare(stored.name, Qt::CaseInsensitive) == 0)
            papers.removeAt(i);
    }
    stored.custom = true;
    papers.append(stored);
    writeAll(*m_settings, papers);
    if (m_settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült elmenteni."));
    return Result<void>();
}

Result<void> CustomPaperStore::remove(const QString &name)
{
    const QString trimmed = name.trimmed();
    const auto existing = readAll();
    if (!existing)
        return existing.error();
    QList<PaperSpec> papers = existing.value();
    bool matched = false;
    for (int i = papers.size() - 1; i >= 0; --i) {
        if (papers.at(i).name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            papers.removeAt(i);
            matched = true;
        }
    }
    if (!matched) {
        return error(QStringLiteral("paper-missing"),
                     QCoreApplication::translate("PhotoPrint", "Ezt a papírt nem találjuk."));
    }
    writeAll(*m_settings, papers);
    if (m_settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült törölni."));
    return Result<void>();
}

} // namespace pp
