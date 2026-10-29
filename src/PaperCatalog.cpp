#include "PaperCatalog.h"

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
        settings.setValue(QStringLiteral("type"), paperTypeKey(paper.type));
        settings.setValue(QStringLiteral("borderless"), paper.borderlessSupported);
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
        return QStringLiteral("Fényes");
    case PaperType::Matte:
        return QStringLiteral("Matt");
    case PaperType::Plain:
        break;
    }
    return QStringLiteral("Normál");
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

QList<PaperSpec> CustomPaperStore::all() const
{
    QList<PaperSpec> papers;
    const int count = m_settings->beginReadArray(QStringLiteral("customPapers"));
    for (int i = 0; i < count; ++i) {
        m_settings->setArrayIndex(i);
        PaperSpec paper;
        paper.name = m_settings->value(QStringLiteral("name")).toString().trimmed();
        paper.physicalWidthMm = m_settings->value(QStringLiteral("widthMm")).toDouble();
        paper.physicalHeightMm = m_settings->value(QStringLiteral("heightMm")).toDouble();
        paper.type = paperTypeFromKey(m_settings->value(QStringLiteral("type")).toString());
        paper.borderlessSupported = m_settings->value(QStringLiteral("borderless")).toBool();
        paper.custom = true;
        if (paper.name.isEmpty() || !(paper.physicalWidthMm > 0.0)
            || !(paper.physicalHeightMm > 0.0))
            continue;
        papers.append(paper);
    }
    m_settings->endArray();
    return papers;
}

Result<void> CustomPaperStore::save(const PaperSpec &paper)
{
    if (paper.name.trimmed().isEmpty())
        return error(QStringLiteral("paper-name"), QStringLiteral("Adj nevet a papírnak."));
    if (!(paper.physicalWidthMm > 0.0) || !(paper.physicalHeightMm > 0.0))
        return error(QStringLiteral("paper-size"), QStringLiteral("A papírméret érvénytelen."));

    QList<PaperSpec> papers = all();
    for (int i = papers.size() - 1; i >= 0; --i) {
        if (papers.at(i).name.compare(paper.name, Qt::CaseInsensitive) == 0)
            papers.removeAt(i);
    }
    PaperSpec stored = paper;
    stored.custom = true;
    papers.append(stored);
    writeAll(*m_settings, papers);
    if (m_settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QStringLiteral("A papírt nem sikerült elmenteni."));
    return Result<void>();
}

Result<void> CustomPaperStore::remove(const QString &name)
{
    QList<PaperSpec> papers = all();
    for (int i = papers.size() - 1; i >= 0; --i) {
        if (papers.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            papers.removeAt(i);
    }
    writeAll(*m_settings, papers);
    if (m_settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QStringLiteral("A papírt nem sikerült törölni."));
    return Result<void>();
}

} // namespace pp
