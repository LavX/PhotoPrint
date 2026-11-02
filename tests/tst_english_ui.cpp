#include "AboutDialog.h"
#include "Document.h"
#include "EffectBrowserDialog.h"
#include "MainWindow.h"
#include "ProjectStore.h"
#include "SheetToolsDialog.h"
#include "TemplateBrowserDialog.h"
#include "Translation.h"

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGroupBox>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QtTest>

using namespace pp;

Q_DECLARE_METATYPE(pp::LoadedProject)

namespace {

/// One user-visible string and the widget that carries it.
struct VisibleText {
    QString where;
    QString text;
};

/// The Hungarian letters that never belong in an English interface.
bool hasHungarianAccent(const QString &text)
{
    static const QString accents = QStringLiteral("áéíóöőúüűÁÉÍÓÖŐÚÜŰ");
    for (const QChar character : text) {
        if (accents.contains(character))
            return true;
    }
    return false;
}

/// The same text with the Hungarian accents folded away, so the word list below
/// only has to carry one spelling per stem.
QString folded(const QString &text)
{
    static const QString from = QStringLiteral("áéíóöőúüűÁÉÍÓÖŐÚÜŰ");
    static const QString to = QStringLiteral("aeiooouuuAEIOOOUUU");
    QString out = text;
    for (int i = 0; i < from.size(); ++i)
        out.replace(from.at(i), to.at(i));
    return out.toLower();
}

/// Hungarian words that carry no accent, so the accent check above cannot see
/// them. They are short enough to collide with English, so they only fire when
/// the whole label is that word: "Be" is a label, "will be" is a sentence.
const QStringList &hungarianWholeWords()
{
    static const QStringList words = {
        QStringLiteral("be"),   QStringLiteral("ki"),   QStringLiteral("fel"),
        QStringLiteral("le"),   QStringLiteral("nem"),  QStringLiteral("igen"),
        QStringLiteral("uj"),   QStringLiteral("db"),   QStringLiteral("lap"),
        QStringLiteral("bal"),  QStringLiteral("jobb"), QStringLiteral("szel"),
        QStringLiteral("szog"), QStringLiteral("kek"),
    };
    return words;
}

/// Longer Hungarian stems, matched with word boundaries so "Overlap" or
/// "keeper" can never fire. Every entry is accented in its real spelling; the
/// list carries the folded form because the accents are caught above.
const QStringList &hungarianStems()
{
    static const QStringList stems = {
        QStringLiteral("foto"),       QStringLiteral("kep"),        QStringLiteral("hely"),
        QStringLiteral("oldal"),      QStringLiteral("szin"),       QStringLiteral("meret"),
        QStringLiteral("nyomtat"),    QStringLiteral("beallit"),    QStringLiteral("szegely"),
        QStringLiteral("rendez"),     QStringLiteral("sokszorosit"), QStringLiteral("mentes"),
        QStringLiteral("megnyit"),    QStringLiteral("torles"),     QStringLiteral("elrendez"),
        QStringLiteral("sablon"),     QStringLiteral("vagojel"),    QStringLiteral("margo"),
        QStringLiteral("hezag"),      QStringLiteral("elonezet"),   QStringLiteral("kivalaszt"),
        QStringLiteral("valassz"),    QStringLiteral("hozzaad"),    QStringLiteral("darab"),
        QStringLiteral("megse"),      QStringLiteral("bezar"),      QStringLiteral("alkalmaz"),
        QStringLiteral("kereses"),    QStringLiteral("kategoria"),  QStringLiteral("effekt"),
        QStringLiteral("papir"),      QStringLiteral("mappa"),      QStringLiteral("kepernyo"),
        QStringLiteral("vagas"),      QStringLiteral("szelesseg"),  QStringLiteral("magassag"),
        QStringLiteral("forgat"),     QStringLiteral("nagyitas"),   QStringLiteral("kicsinyit"),
        QStringLiteral("igazit"),     QStringLiteral("felso"),      QStringLiteral("kozep"),
        QStringLiteral("elore"),      QStringLiteral("hatra"),      QStringLiteral("nincs"),
        QStringLiteral("projekt"),    QStringLiteral("egyedi"),     QStringLiteral("automatikus"),
        QStringLiteral("fekete"),     QStringLiteral("feher"),      QStringLiteral("piros"),
        QStringLiteral("zold"),       QStringLiteral("sarga"),      QStringLiteral("szurke"),
        QStringLiteral("narancs"),    QStringLiteral("barna"),      QStringLiteral("lila"),
    };
    return stems;
}

/// The strings that are meant to read the same in every language: the brand,
/// values made of digits and units only, and the paper names the catalogue
/// ships. Nothing here may contain a Hungarian word.
bool isTechnicalOrBrand(const QString &text)
{
    if (text == QStringLiteral("PhotoPrint"))
        return true;
    // The window title is the brand plus the project's file name, which is the
    // user's data and is not translated.
    if (text.startsWith(QStringLiteral("PhotoPrint —"))
        || text.startsWith(QStringLiteral("PhotoPrint *"))
        || text.startsWith(QStringLiteral("PhotoPrint –"))) {
        return true;
    }

    bool hasLetter = false;
    for (const QChar character : text) {
        if (character.isLetter())
            hasLetter = true;
    }
    if (!hasLetter)
        return true; // 50%, 100 × 150, ‹, ›, +, ° and the like

    static const QRegularExpression paper(
        QStringLiteral("^(A[3-6]|Letter|Legal)( \\([0-9.,]+ × [0-9.,]+ cm\\))?$"));
    if (paper.match(text).hasMatch())
        return true;
    static const QRegularExpression size(
        QStringLiteral("^[0-9.,]+ × [0-9.,]+ (cm|mm)$"));
    return size.match(text).hasMatch();
}

bool looksHungarian(const QString &text)
{
    if (hasHungarianAccent(text))
        return true;
    // A spin box suffix carries its separating space; the word itself is what
    // matters, so the comparison is made on the trimmed text.
    const QString plain = folded(text.trimmed());
    for (const QString &word : hungarianWholeWords()) {
        if (plain == word)
            return true;
    }
    for (const QString &stem : hungarianStems()) {
        if (QRegularExpression(QStringLiteral("\\b") + stem + QStringLiteral("\\b"))
                .match(plain)
                .hasMatch()) {
            return true;
        }
    }
    return false;
}

void addText(const QString &where, const QString &text, QList<VisibleText> *out)
{
    if (text.trimmed().isEmpty())
        return;
    out->append({where, text});
}

void collectAction(QAction *action, const QString &where, QList<VisibleText> *out)
{
    if (!action)
        return;
    addText(where + QStringLiteral("[action]"), action->text(), out);
    addText(where + QStringLiteral("[action-tooltip]"), action->toolTip(), out);
    addText(where + QStringLiteral("[action-statustip]"), action->statusTip(), out);
    addText(where + QStringLiteral("[action-whatsthis]"), action->whatsThis(), out);
}

/// Walks the whole widget tree: every label, button, list entry, tab, spin
/// suffix, accessible name, tooltip and the actions attached to each widget.
/// Object names are internal and never read; the walked path only labels where
/// a finding sits.
void collectWidget(QWidget *widget, const QString &path, QList<VisibleText> *out)
{
    const QString name = widget->objectName().isEmpty()
                             ? QString::fromLatin1(widget->metaObject()->className())
                             : widget->objectName();
    const QString here = path + QLatin1Char('/') + name;

    addText(here + QStringLiteral("[window-title]"), widget->windowTitle(), out);
    addText(here + QStringLiteral("[tooltip]"), widget->toolTip(), out);
    addText(here + QStringLiteral("[statustip]"), widget->statusTip(), out);
    addText(here + QStringLiteral("[whatsthis]"), widget->whatsThis(), out);
    addText(here + QStringLiteral("[accessible-name]"), widget->accessibleName(), out);
    addText(here + QStringLiteral("[accessible-description]"), widget->accessibleDescription(),
            out);

    if (auto *label = qobject_cast<QLabel *>(widget))
        addText(here + QStringLiteral("[text]"), label->text(), out);
    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        addText(here + QStringLiteral("[button]"), button->text(), out);
    if (auto *group = qobject_cast<QGroupBox *>(widget))
        addText(here + QStringLiteral("[group-title]"), group->title(), out);
    if (auto *menu = qobject_cast<QMenu *>(widget))
        addText(here + QStringLiteral("[menu-title]"), menu->title(), out);
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        addText(here + QStringLiteral("[placeholder]"), combo->placeholderText(), out);
#endif
        for (int index = 0; index < combo->count(); ++index)
            addText(here + QStringLiteral("[item]"), combo->itemText(index), out);
    }
    if (auto *list = qobject_cast<QListWidget *>(widget)) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            addText(here + QStringLiteral("[item]"), item->text(), out);
            addText(here + QStringLiteral("[item-tooltip]"), item->toolTip(), out);
        }
    }
    if (auto *tabs = qobject_cast<QTabWidget *>(widget)) {
        for (int index = 0; index < tabs->count(); ++index)
            addText(here + QStringLiteral("[tab]"), tabs->tabText(index), out);
    }
    if (auto *edit = qobject_cast<QLineEdit *>(widget))
        addText(here + QStringLiteral("[placeholder]"), edit->placeholderText(), out);
    if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
        addText(here + QStringLiteral("[prefix]"), spin->prefix(), out);
        addText(here + QStringLiteral("[suffix]"), spin->suffix(), out);
        addText(here + QStringLiteral("[special-value]"), spin->specialValueText(), out);
    }
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
        addText(here + QStringLiteral("[prefix]"), spin->prefix(), out);
        addText(here + QStringLiteral("[suffix]"), spin->suffix(), out);
        addText(here + QStringLiteral("[special-value]"), spin->specialValueText(), out);
    }
    if (auto *status = qobject_cast<QStatusBar *>(widget))
        addText(here + QStringLiteral("[message]"), status->currentMessage(), out);
    if (auto *toolbar = qobject_cast<QToolBar *>(widget))
        addText(here + QStringLiteral("[toolbar-title]"), toolbar->windowTitle(), out);

    for (QAction *action : widget->actions())
        collectAction(action, here, out);
    for (QObject *child : widget->children()) {
        if (auto *childWidget = qobject_cast<QWidget *>(child))
            collectWidget(childWidget, here, out);
    }
}

/// Fails with every distinct offender and the widget that carries it. The
/// message never prints during a passing run. @p minimumTexts keeps the walk
/// honest: a surface that suddenly yields nothing would otherwise pass without
/// looking at anything.
void verifyNoHungarian(QWidget *root, const QString &surface, int minimumTexts)
{
    QList<VisibleText> texts;
    collectWidget(root, QStringLiteral("root"), &texts);
    QVERIFY2(texts.size() >= minimumTexts,
             qPrintable(QStringLiteral("%1 yielded only %2 visible strings, expected at least %3")
                            .arg(surface)
                            .arg(texts.size())
                            .arg(minimumTexts)));

    QStringList offenders;
    QSet<QString> reported;
    for (const VisibleText &entry : texts) {
        if (isTechnicalOrBrand(entry.text) || !looksHungarian(entry.text))
            continue;
        if (reported.contains(entry.text))
            continue;
        reported.insert(entry.text);
        offenders.append(QStringLiteral("  %1 | %2").arg(entry.where, entry.text));
    }
    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("%1 still shows Hungarian or untranslated text:\n%2")
                            .arg(surface, offenders.join(QLatin1Char('\n')))));
}

QString makePhoto(const QString &directory, const QString &name, const QColor &colour)
{
    QImage image(320, 240, QImage::Format_RGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(colour);
    const QString path = QDir(directory).filePath(name);
    QImageWriter writer(path, "png");
    writer.write(image);
    return path;
}

/// A page with four empty cells in a 2 x 2 grid, loaded through the normal
/// project path so the window builds the layout panel from real state.
DocumentSnapshot fourEmptySlots()
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-labels");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;
    page.slotList = {
        Slot{QStringLiteral("slot-labels-0"), QRectF(0.0, 0.0, 0.5, 0.5)},
        Slot{QStringLiteral("slot-labels-1"), QRectF(0.5, 0.0, 0.5, 0.5)},
        Slot{QStringLiteral("slot-labels-2"), QRectF(0.0, 0.5, 0.5, 0.5)},
        Slot{QStringLiteral("slot-labels-3"), QRectF(0.5, 0.5, 0.5, 0.5)},
    };
    page.layoutId = QStringLiteral("n04-grid-2x2");
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

bool loadDocument(MainWindow &window, const QTemporaryDir &dir, const DocumentSnapshot &document)
{
    const QString path = dir.filePath(QStringLiteral("labels.ppj"));
    ProjectStore store;
    if (!store.save(path, document, SaveMode::Normal).ok())
        return false;
    const auto loaded = store.load(path);
    if (!loaded)
        return false;
    qRegisterMetaType<pp::LoadedProject>("pp::LoadedProject");
    if (!QMetaObject::invokeMethod(&window, "applyLoadedProject", Qt::DirectConnection,
                                   Q_ARG(pp::LoadedProject, loaded.value()), Q_ARG(bool, false))) {
        return false;
    }
    return window.waitForPreview(15000);
}

/// A two-photo page for the sheet tools: one landscape, one portrait, both
/// inside the paper.
DocumentSnapshot twoPhotoPage(const QString &firstPath, const QString &secondPath)
{
    DocumentSnapshot document;
    Page page;
    page.id = QStringLiteral("page-sheet-labels");
    page.paper.name = QStringLiteral("A4");
    page.paper.physicalWidthMm = 210.0;
    page.paper.physicalHeightMm = 297.0;

    Frame left;
    left.id = QStringLiteral("frame-labels-0");
    left.sourcePath = firstPath;
    left.cx = 60.0;
    left.cy = 80.0;
    left.width = 80.0;
    left.height = 60.0;
    left.fit = FitMode::Fill;
    left.autoPlaced = false;

    Frame right;
    right.id = QStringLiteral("frame-labels-1");
    right.sourcePath = secondPath;
    right.cx = 150.0;
    right.cy = 220.0;
    right.width = 55.0;
    right.height = 80.0;
    right.fit = FitMode::Fill;
    right.autoPlaced = false;

    page.frames = {left, right};
    document.pages.append(page);
    document.currentPage = 0;
    return document;
}

bool isIdentifierChar(QChar character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}

/// The source with comments blanked out and raw strings emptied, so the string
/// literal pass below only sees real code and real string literals.
QString blankedSource(const QString &source)
{
    QString out = source;
    const int size = out.size();
    int i = 0;
    while (i < size) {
        const QChar character = out.at(i);
        if (character == QLatin1Char('"') && i > 0
            && out.at(i - 1) == QLatin1Char('R')
            && (i == 1 || !isIdentifierChar(out.at(i - 2)))) {
            const int open = out.indexOf(QLatin1Char('('), i + 1);
            if (open < 0)
                break;
            const QString delimiter = out.mid(i + 1, open - i - 1);
            const QString terminator = QStringLiteral(")") + delimiter + QStringLiteral("\"");
            int end = out.indexOf(terminator, open + 1);
            end = end < 0 ? size : end + terminator.size();
            for (int k = i; k < end; ++k) {
                if (out.at(k) != QLatin1Char('\n'))
                    out[k] = QLatin1Char(' ');
            }
            i = end;
        } else if (character == QLatin1Char('"')) {
            ++i;
            while (i < size) {
                if (out.at(i) == QLatin1Char('\\')) {
                    i += 2;
                    continue;
                }
                if (out.at(i) == QLatin1Char('"')) {
                    ++i;
                    break;
                }
                ++i;
            }
        } else if (character == QLatin1Char('\'')) {
            int end = i + 1;
            bool closed = false;
            while (end < size && out.at(end) != QLatin1Char('\n')) {
                if (out.at(end) == QLatin1Char('\\')) {
                    end += 2;
                    continue;
                }
                if (out.at(end) == QLatin1Char('\'')) {
                    ++end;
                    closed = true;
                    break;
                }
                ++end;
            }
            if (closed) {
                for (int k = i; k < end; ++k)
                    out[k] = QLatin1Char(' ');
                i = end;
            } else {
                ++i;
            }
        } else if (out.mid(i, 2) == QStringLiteral("//")) {
            int end = out.indexOf(QLatin1Char('\n'), i);
            end = end < 0 ? size : end;
            for (int k = i; k < end; ++k)
                out[k] = QLatin1Char(' ');
            i = end;
        } else if (out.mid(i, 2) == QStringLiteral("/*")) {
            int end = out.indexOf(QStringLiteral("*/"), i + 2);
            end = end < 0 ? size : end + 2;
            for (int k = i; k < end; ++k) {
                if (out.at(k) != QLatin1Char('\n'))
                    out[k] = QLatin1Char(' ');
            }
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

struct SourceLiteral {
    int position = 0;
    int end = 0;
    QString text;
};

/// Every string literal in a comment-free source, with adjacent literals
/// joined, because a sentence may be split over several lines.
QList<SourceLiteral> sourceLiterals(const QString &source)
{
    QList<SourceLiteral> raw;
    const int size = source.size();
    int i = 0;
    while (i < size) {
        if (source.at(i) != QLatin1Char('"')) {
            ++i;
            continue;
        }
        const int start = i;
        QString content;
        ++i;
        while (i < size) {
            const QChar character = source.at(i);
            if (character == QLatin1Char('\\') && i + 1 < size) {
                // The escape only matters for \" and \\; the rest is text.
                content.append(source.at(i + 1));
                i += 2;
                continue;
            }
            if (character == QLatin1Char('"')) {
                ++i;
                break;
            }
            content.append(character);
            ++i;
        }
        raw.append({start, i, content});
    }

    QList<SourceLiteral> merged;
    for (int index = 0; index < raw.size(); ++index) {
        SourceLiteral literal = raw.at(index);
        while (index + 1 < raw.size()
               && source.mid(literal.end, raw.at(index + 1).position - literal.end).trimmed().isEmpty()) {
            ++index;
            literal.text += raw.at(index).text;
            literal.end = raw.at(index).end;
        }
        merged.append(literal);
    }
    return merged;
}

/// The identifier of the call whose argument list contains @p position, or an
/// empty string when the literal is not an argument.
QString enclosingCall(const QString &source, int position)
{
    int depth = 0;
    for (int i = position - 1; i >= 0; --i) {
        const QChar character = source.at(i);
        if (character == QLatin1Char(')')) {
            ++depth;
        } else if (character == QLatin1Char('(')) {
            if (depth == 0) {
                int end = i - 1;
                while (end >= 0 && source.at(end).isSpace())
                    --end;
                int start = end;
                while (start >= 0
                       && (isIdentifierChar(source.at(start)) || source.at(start) == QLatin1Char(':'))) {
                    --start;
                }
                return source.mid(start + 1, end - start);
            }
            --depth;
        } else if (depth == 0
                   && (character == QLatin1Char(';') || character == QLatin1Char('{')
                       || character == QLatin1Char('}'))) {
            return {};
        }
    }
    return {};
}

QHash<QString, QImage> previewsFor(const DocumentSnapshot &document)
{
    QHash<QString, QImage> previews;
    for (const Page &page : document.pages) {
        for (const Frame &frame : page.frames) {
            QImage image(240, 180, QImage::Format_RGB32);
            image.fill(QColor(90, 130, 170));
            previews.insert(frame.id, image);
        }
    }
    return previews;
}

} // namespace

class TestEnglishUi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void mainWindowEmptyPage();
    void mainWindowWithPhotosAndSelection();
    void mainWindowLayoutPageAndEmptySlot();
    void effectBrowser();
    void templateBrowser();
    void sheetToolsBatch();
    void sheetToolsAlign();
    void aboutDialog();

    void everyTranslatedMessageHasAnEnglishText();
    void sourceHasNoUntranslatedHungarianText();

private:
    QTemporaryDir m_settingsDir;
};

void TestEnglishUi::initTestCase()
{
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PhotoPrintTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PhotoPrintEnglishUiTest"));
    QSettings().clear();
    qputenv("PHOTOPRINT_NO_RECOVERY", "1");
    qputenv("PHOTOPRINT_NO_SETTINGS", "1");
    qputenv("PHOTOPRINT_NO_CUPS", "1");

    // English is the language under test; every surface below is built after
    // this, so the translated text is what the walk sees.
    QVERIFY(i18n::installLanguage(QStringLiteral("en")));
    QCOMPARE(i18n::activeLanguage(), QStringLiteral("en"));
}

void TestEnglishUi::mainWindowEmptyPage()
{
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(150);

    verifyNoHungarian(&window, QStringLiteral("the empty main window"), 150);
}

void TestEnglishUi::mainWindowWithPhotosAndSelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList photos = {
        makePhoto(dir.path(), QStringLiteral("one.png"), QColor(70, 130, 170)),
        makePhoto(dir.path(), QStringLiteral("two.png"), QColor(160, 110, 70)),
    };

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(QMetaObject::invokeMethod(&window, "addDroppedFiles", Qt::DirectConnection,
                                      Q_ARG(QStringList, photos)));
    QVERIFY(window.waitForPreview(15000));
    QTest::qWait(150);

    // A selected photograph opens the photo inspector and its effect row.
    QVERIFY(!window.document().pages.isEmpty());
    QVERIFY(!window.document().pages.first().frames.isEmpty());
    const QString frameId = window.document().pages.first().frames.first().id;
    QVERIFY(QMetaObject::invokeMethod(&window, "selectFrame", Qt::DirectConnection,
                                      Q_ARG(QString, frameId), Q_ARG(bool, false)));
    QTest::qWait(200);

    verifyNoHungarian(&window, QStringLiteral("the main window with a selected photo"), 150);
}

void TestEnglishUi::mainWindowLayoutPageAndEmptySlot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QTest::qWait(120);
    QVERIFY(loadDocument(window, dir, fourEmptySlots()));
    QTest::qWait(300);

    verifyNoHungarian(&window, QStringLiteral("the laid-out page"), 150);

    // An empty cell selected: the layout panel's counts and the hint line.
    auto *view = window.findChild<PageView *>();
    QVERIFY(view);
    const Page page = window.document().pages.first();
    QVERIFY(page.hasLayout());
    const QString slot = page.slotList.at(1).id;
    QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                      view->slotRect(slot).center().toPoint());
    QCOMPARE(view->selectedSlot(), slot);
    QTest::qWait(200);

    verifyNoHungarian(&window, QStringLiteral("the selected empty slot"), 150);
}

void TestEnglishUi::effectBrowser()
{
    EffectBrowserRequest request;
    QImage sample(240, 180, QImage::Format_RGB32);
    sample.fill(QColor(120, 90, 60));
    request.sample = sample;
    request.sampleHash = QStringLiteral("labels-sample");

    EffectBrowserDialog dialog(request);
    dialog.show();
    QTest::qWait(300);

    verifyNoHungarian(&dialog, QStringLiteral("the effect browser"), 40);
    dialog.close();
}

void TestEnglishUi::templateBrowser()
{
    TemplateBrowserRequest request;
    request.paper.name = QStringLiteral("A4");
    request.paper.physicalWidthMm = 210.0;
    request.paper.physicalHeightMm = 297.0;

    Frame first;
    first.id = QStringLiteral("template-frame-0");
    first.width = 80.0;
    first.height = 60.0;
    Frame second;
    second.id = QStringLiteral("template-frame-1");
    second.width = 60.0;
    second.height = 80.0;
    request.frames = {first, second};
    QImage thumbnail(160, 120, QImage::Format_RGB32);
    thumbnail.fill(QColor(80, 120, 160));
    request.photos = {thumbnail, thumbnail};

    TemplateBrowserDialog dialog(request);
    dialog.show();
    QTest::qWait(300);

    verifyNoHungarian(&dialog, QStringLiteral("the template browser"), 30);
    dialog.close();
}

void TestEnglishUi::sheetToolsBatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first =
        makePhoto(dir.path(), QStringLiteral("sheet-one.png"), QColor(75, 130, 175));
    const QString second =
        makePhoto(dir.path(), QStringLiteral("sheet-two.png"), QColor(175, 130, 75));
    const DocumentSnapshot document = twoPhotoPage(first, second);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Batch, document, 0,
                            document.pages.first().frames.first(), previewsFor(document));
    dialog.show();
    QTest::qWait(300);

    verifyNoHungarian(&dialog, QStringLiteral("the batch copies dialog"), 60);
    dialog.close();
}

void TestEnglishUi::sheetToolsAlign()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first =
        makePhoto(dir.path(), QStringLiteral("align-one.png"), QColor(75, 130, 175));
    const QString second =
        makePhoto(dir.path(), QStringLiteral("align-two.png"), QColor(175, 130, 75));
    const DocumentSnapshot document = twoPhotoPage(first, second);

    SheetToolsDialog dialog(SheetToolsDialog::Mode::Align, document, 0, std::nullopt,
                            previewsFor(document));
    dialog.show();
    QTest::qWait(300);

    verifyNoHungarian(&dialog, QStringLiteral("the align photos dialog"), 60);
    dialog.close();
}

void TestEnglishUi::aboutDialog()
{
    AboutDialog dialog;
    dialog.show();
    QTest::qWait(100);

    verifyNoHungarian(&dialog, QStringLiteral("the about dialog"), 15);
    dialog.close();
}

void TestEnglishUi::everyTranslatedMessageHasAnEnglishText()
{
    // The static half of the sweep: every message lupdate extracted from src
    // has an English entry, and it is neither empty nor the Hungarian source.
    // The four paper size names are identical in every language, and are the
    // only entries allowed to read exactly like their source.
    QFile file(QStringLiteral(":/i18n/photoprint_en.ts"));
    if (!file.exists()) {
        // The .ts is not embedded; the compiled .qm is what ships. The source
        // catalogue is read from the repository instead.
        file.setFileName(QStringLiteral(PHOTOPRINT_SOURCE_DIR "/resources/i18n/photoprint_en.ts"));
    }
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));

    const QString xml = QString::fromUtf8(file.readAll());
    static const QRegularExpression message(
        QStringLiteral("<message[^>]*>(.*?)</message>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression source(
        QStringLiteral("<source>(.*?)</source>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression translation(
        QStringLiteral("<translation[^>]*>(.*?)</translation>"),
        QRegularExpression::DotMatchesEverythingOption);

    const QStringList identicalByDesign = {
        QStringLiteral("50 × 70 mm"),
        QStringLiteral("40 × 40 mm"),
        QStringLiteral("30 × 40 mm"),
        QStringLiteral("25 × 35 mm"),
    };

    QStringList problems;
    int count = 0;
    auto it = message.globalMatch(xml);
    while (it.hasNext()) {
        const QString body = it.next().captured(1);
        const QString sourceText = source.match(body).captured(1);
        if (sourceText.isEmpty())
            continue;
        ++count;
        const QRegularExpressionMatch translated = translation.match(body);
        if (!translated.hasMatch()) {
            problems.append(QStringLiteral("%1: no translation").arg(sourceText));
            continue;
        }
        const QString english = translated.captured(1).trimmed();
        if (english.isEmpty())
            problems.append(QStringLiteral("%1: empty").arg(sourceText));
        else if (english == sourceText && !identicalByDesign.contains(sourceText))
            problems.append(QStringLiteral("%1: still the Hungarian source").arg(sourceText));
    }
    QVERIFY2(count > 300, qPrintable(QStringLiteral("only %1 messages parsed").arg(count)));
    QVERIFY2(problems.isEmpty(),
             qPrintable(QStringLiteral("the English catalogue falls back:\n  %1")
                            .arg(problems.join(QStringLiteral("\n  ")))));
}

void TestEnglishUi::sourceHasNoUntranslatedHungarianText()
{
    // The static half of the sweep. A Hungarian literal that is not an argument
    // of a translate call can never reach a catalogue, so it would stay
    // Hungarian in every language. Diagnostics are the one exception: a
    // qWarning goes to the log, not to the interface.
    const QString root = QStringLiteral(PHOTOPRINT_SOURCE_DIR "/src");
    const QSet<QString> translatedCalls = {
        QStringLiteral("translate"),
        QStringLiteral("QCoreApplication::translate"),
        QStringLiteral("QT_TRANSLATE_NOOP"),
        QStringLiteral("QT_TRANSLATE_NOOP3"),
        QStringLiteral("QT_TR_NOOP"),
        QStringLiteral("tr"),
    };
    const QSet<QString> diagnosticCalls = {
        QStringLiteral("qWarning"), QStringLiteral("qDebug"), QStringLiteral("qInfo"),
        QStringLiteral("qCritical"), QStringLiteral("qFatal"),
    };

    QStringList offenders;
    int scanned = 0;
    QDirIterator it(root, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
                   QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(path));
        const QString source = blankedSource(QString::fromUtf8(file.readAll()));
        for (const SourceLiteral &literal : sourceLiterals(source)) {
            if (!looksHungarian(literal.text))
                continue;
            const QString call = enclosingCall(source, literal.position);
            if (translatedCalls.contains(call) || diagnosticCalls.contains(call))
                continue;
            const int line = source.left(literal.position).count(QLatin1Char('\n')) + 1;
            offenders.append(QStringLiteral("  %1:%2 | %3 | %4")
                                 .arg(QDir(root).relativeFilePath(path))
                                 .arg(line)
                                 .arg(call.isEmpty() ? QStringLiteral("no call") : call,
                                      literal.text));
        }
        ++scanned;
    }
    QVERIFY2(scanned > 20, qPrintable(QStringLiteral("only %1 source files scanned").arg(scanned)));
    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("Hungarian text outside a translate call:\n%1")
                            .arg(offenders.join(QLatin1Char('\n')))));
}

QTEST_MAIN(TestEnglishUi)
#include "tst_english_ui.moc"
