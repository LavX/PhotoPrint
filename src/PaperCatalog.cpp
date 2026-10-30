#include "PaperCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>

#include <cmath>
#include <memory>

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

/// A millimetre value that can describe a real sheet. Infinite is excluded:
/// +inf passes a bare `> 0` test, then produces an empty or infinite rectangle
/// everywhere downstream.
bool usablePhysicalMm(double value)
{
    return std::isfinite(value) && value > 0.0;
}

/// A declared logical axis. Zero means "not declared" and is valid; anything
/// negative or non-finite is not a measurement.
bool usableLogicalMm(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

bool isUsableStoredPaper(const PaperSpec &paper)
{
    return !paper.name.isEmpty() && usablePhysicalMm(paper.physicalWidthMm)
        && usablePhysicalMm(paper.physicalHeightMm) && usableLogicalMm(paper.logicalWidthMm)
        && usableLogicalMm(paper.logicalHeightMm);
}

Result<QList<PaperSpec>> corruptPaperFile()
{
    return error(QStringLiteral("paper-corrupt"),
                 QCoreApplication::translate("PhotoPrint",
                                             "A papírbeállítások fájlja sérült."));
}

/// Write lock for one store file, shared by every instance (and every copy of
/// the app) that rewrites it. save() and remove() are read-modify-write cycles,
/// so without it two writers silently drop each other's papers.
///
/// The name is deliberately not "<file>.lock": QSettings takes that lock itself
/// while it syncs a file, so a lock of our own on the same name would wait for
/// the QSettings lock inside writeAll() and freeze the caller.
std::unique_ptr<QLockFile> acquireWriteLock(const QString &lockPath)
{
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath()))
        return nullptr;
    auto lock = std::make_unique<QLockFile>(lockPath);
    // Called on the GUI thread: fail fast with the real reason instead of
    // freezing the window behind another instance's lock.
    if (!lock->tryLock(250))
        return nullptr;
    return lock;
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
    : m_settingsFile(settingsFile)
{
}

CustomPaperStore::~CustomPaperStore() = default;

std::unique_ptr<QSettings> CustomPaperStore::openSettings() const
{
    // A new object per operation, on purpose: see the header. Constructing it is
    // cheap and does not read the file until the first value access.
    if (m_settingsFile.isEmpty())
        return std::make_unique<QSettings>();
    return std::make_unique<QSettings>(m_settingsFile, QSettings::IniFormat);
}

QString CustomPaperStore::writeLockPath() const
{
    const QString fileName = m_settingsFile.isEmpty() ? QSettings().fileName() : m_settingsFile;
    return fileName + QStringLiteral(".papers.lock");
}

Result<QList<PaperSpec>> CustomPaperStore::readAll() const
{
    const std::unique_ptr<QSettings> settings = openSettings();
    QList<PaperSpec> papers;
    bool damaged = false;
    const int count = settings->beginReadArray(QStringLiteral("customPapers"));
    for (int i = 0; i < count; ++i) {
        settings->setArrayIndex(i);
        PaperSpec paper;
        paper.name = settings->value(QStringLiteral("name")).toString().trimmed();
        paper.physicalWidthMm = settings->value(QStringLiteral("widthMm")).toDouble();
        paper.physicalHeightMm = settings->value(QStringLiteral("heightMm")).toDouble();
        // A missing logical size means "same as the physical size".
        paper.logicalWidthMm = settings->value(QStringLiteral("logicalWidthMm")).toDouble();
        paper.logicalHeightMm = settings->value(QStringLiteral("logicalHeightMm")).toDouble();
        paper.type = paperTypeFromKey(settings->value(QStringLiteral("type")).toString());
        paper.borderlessSupported = settings->value(QStringLiteral("borderless")).toBool();
        paper.hasCheckedOffset = settings->value(QStringLiteral("hasCheckedOffset")).toBool();
        paper.offsetXMm = settings->value(QStringLiteral("offsetXMm")).toDouble();
        paper.offsetYMm = settings->value(QStringLiteral("offsetYMm")).toDouble();
        // Everything in this store was defined by the user: the flag follows
        // from the store, it is not a second piece of information to keep.
        paper.custom = true;
        // An entry the app would never write (no name, no usable size) is
        // damage, not a paper to drop quietly: the array still claims it exists.
        if (!isUsableStoredPaper(paper)) {
            damaged = true;
            break;
        }
        papers.append(paper);
    }
    settings->endArray();

    if (damaged)
        return corruptPaperFile();

    // A list rebuilt from a half-read file would be written straight back by the
    // next save(), and every paper the file still held would be gone. Report the
    // damage instead of pretending the user has no papers.
    switch (settings->status()) {
    case QSettings::NoError:
        break;
    case QSettings::FormatError:
        return corruptPaperFile();
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
        for (const QString &key : settings->allKeys()) {
            if (key.startsWith(prefix) && key != prefix + QStringLiteral("size"))
                return corruptPaperFile();
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
    // A size that cannot describe a sheet (zero, negative, infinite, NaN) is
    // rejected here, so the file never holds a paper no geometry can draw.
    if (!isUsableStoredPaper(stored))
        return error(QStringLiteral("paper-size"), QCoreApplication::translate("PhotoPrint", "A papírméret érvénytelen."));

    // The lock comes first: the settings object below is created after it, so
    // the read-modify-write cannot be based on a snapshot taken before another
    // writer finished.
    const std::unique_ptr<QLockFile> lock = acquireWriteLock(writeLockPath());
    if (!lock)
        return error(QStringLiteral("paper-lock"),
                     QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült elmenteni."));
    const std::unique_ptr<QSettings> settings = openSettings();

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
    writeAll(*settings, papers);
    if (settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült elmenteni."));
    return Result<void>();
}

Result<void> CustomPaperStore::remove(const QString &name)
{
    const QString trimmed = name.trimmed();
    const std::unique_ptr<QLockFile> lock = acquireWriteLock(writeLockPath());
    if (!lock)
        return error(QStringLiteral("paper-lock"),
                     QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült törölni."));
    const std::unique_ptr<QSettings> settings = openSettings();

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
    writeAll(*settings, papers);
    if (settings->status() != QSettings::NoError)
        return error(QStringLiteral("paper-write"), QCoreApplication::translate("PhotoPrint", "A papírt nem sikerült törölni."));
    return Result<void>();
}

} // namespace pp
