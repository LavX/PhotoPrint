#include "CanvasController.h"

#include "LayoutEngine.h"
#include "SlotGeometry.h"
#include "SlotLayoutEngine.h"

#include <QCoreApplication>
#include <QSet>
#include <QUndoCommand>

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace pp {

namespace {

struct LocatedFrame {
    int pageIndex = -1;
    int frameIndex = -1;
    Frame frame;
};

LocatedFrame locate(const DocumentSnapshot &snapshot, const QString &frameId)
{
    LocatedFrame located;
    for (int p = 0; p < snapshot.pages.size(); ++p) {
        const Page &page = snapshot.pages.at(p);
        for (int f = 0; f < page.frames.size(); ++f) {
            if (page.frames.at(f).id == frameId) {
                located.pageIndex = p;
                located.frameIndex = f;
                located.frame = page.frames.at(f);
                return located;
            }
        }
    }
    return located;
}

Result<void> validateSlotState(const QList<Slot> &slotValues, const QList<Frame> &frames,
                               const SlotStyle &style)
{
    // The editor may only publish a style it could have produced: an
    // out-of-range margin or gap, or a non-finite radius, is a bug in the
    // caller, not something to sanitise silently behind its back.
    if (!(SlotGeometry::sanitiseStyle(style) == style)) {
        return error(QStringLiteral("layout-slot-style"),
                     QCoreApplication::translate(
                         "PhotoPrint",
                         "A margó, a szegély vagy a sarok lekerekítése érvénytelen."));
    }
    if (slotValues.size() > SlotGeometry::kMaxSlotsPerPage) {
        return error(QStringLiteral("layout-slot-cap"),
                     QCoreApplication::translate("PhotoPrint",
                                                 "Egy lapon legfeljebb 12 hely lehet."));
    }
    QSet<QString> slotIds;
    for (const Slot &slot : slotValues) {
        if (slot.id.isEmpty() || slotIds.contains(slot.id)) {
            return error(QStringLiteral("layout-slot-id"),
                         QCoreApplication::translate(
                             "PhotoPrint",
                             "A hely azonosítója üres vagy ismétlődik."));
        }
        if (!SlotGeometry::unitRectIsValid(slot.unitRect)) {
            return error(QStringLiteral("layout-slot-unit"),
                         QCoreApplication::translate("PhotoPrint",
                                                     "A hely a lapon kívülre esik."));
        }
        slotIds.insert(slot.id);
    }
    QSet<QString> assigned;
    for (const Frame &frame : frames) {
        if (frame.slotId.isEmpty())
            continue;
        if (!slotIds.contains(frame.slotId)) {
            return error(QStringLiteral("layout-slot-dangling"),
                         QCoreApplication::translate(
                             "PhotoPrint",
                             "Egy fotó olyan helyre hivatkozik, amely nem létezik."));
        }
        if (assigned.contains(frame.slotId)) {
            return error(QStringLiteral("layout-slot-duplicate"),
                         QCoreApplication::translate(
                             "PhotoPrint", "Két fotó nem kerülhet ugyanabba a helybe."));
        }
        assigned.insert(frame.slotId);
    }
    return Result<void>();
}

} // namespace

Result<void> validateLayoutEdit(const DocumentSnapshot &document, const ApplyLayoutEdit &edit)
{
    QSet<int> touched;
    for (const PageLayout &change : edit.pages) {
        if (change.pageIndex < 0 || change.pageIndex >= document.pages.size()) {
            return error(QStringLiteral("layout-page-index"),
                         QCoreApplication::translate("PhotoPrint", "Érvénytelen lapsorszám."));
        }
        if (touched.contains(change.pageIndex)) {
            return error(QStringLiteral("layout-page-duplicate"),
                         QCoreApplication::translate(
                             "PhotoPrint", "Egy lap csak egyszer szerepelhet a módosításban."));
        }
        touched.insert(change.pageIndex);

        // The slot invariant is validated on the page the edit produces, not on
        // the edit's fields alone: replacing only the frames or only the slot
        // list must not be able to create a dangling assignment.
        const Page &page = document.pages.at(change.pageIndex);
        const QList<Slot> &slotValues = change.slotList ? *change.slotList : page.slotList;
        const QList<Frame> &frames = change.frames ? *change.frames : page.frames;
        const SlotStyle &style = change.slotStyle ? *change.slotStyle : page.slotStyle;
        auto slotState = validateSlotState(slotValues, frames, style);
        if (!slotState)
            return slotState;
    }

    QSet<QString> pageIds;
    for (const Page &page : document.pages)
        pageIds.insert(page.id);
    QSet<QString> newIds;
    for (const Page &page : edit.newPages) {
        if (page.id.isEmpty()) {
            return error(QStringLiteral("layout-page-id"),
                         QCoreApplication::translate("PhotoPrint",
                                                     "A lap azonosítója nem lehet üres."));
        }
        if (pageIds.contains(page.id)) {
            return error(QStringLiteral("layout-page-id"),
                         QCoreApplication::translate(
                             "PhotoPrint", "Az új lap azonosítója már létezik."));
        }
        if (newIds.contains(page.id)) {
            return error(QStringLiteral("layout-page-id"),
                         QCoreApplication::translate(
                             "PhotoPrint", "Két új lap azonosítója megegyezik."));
        }
        newIds.insert(page.id);
    }

    if (edit.insertIndex < -1 || edit.insertIndex > document.pages.size()) {
        return error(QStringLiteral("layout-insert-index"),
                     QCoreApplication::translate("PhotoPrint", "Érvénytelen beszúrási hely."));
    }

    const int finalPages = int(document.pages.size()) + int(edit.newPages.size());
    if (edit.selectPage < -1 || edit.selectPage >= finalPages) {
        return error(QStringLiteral("layout-select-page"),
                     QCoreApplication::translate("PhotoPrint", "Érvénytelen kiválasztott lap."));
    }
    return Result<void>();
}

class FrameChangeCommand : public QUndoCommand
{
public:
    FrameChangeCommand(CanvasController *controller, QString frameId, Frame after,
                       QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_frameId(std::move(frameId)),
          m_after(std::move(after))
    {
        const LocatedFrame located = locate(m_controller->document(), m_frameId);
        m_before = located.frame;
    }

    void undo() override { m_controller->applyFrame(m_frameId, m_before); }
    void redo() override { m_controller->applyFrame(m_frameId, m_after); }

private:
    CanvasController *m_controller;
    QString m_frameId;
    Frame m_before;
    Frame m_after;
};

class AddFrameCommand : public QUndoCommand
{
public:
    AddFrameCommand(CanvasController *controller, Frame frame, int pageIndex,
                    QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_frame(std::move(frame)),
          m_pageIndex(pageIndex)
    {
    }

    void undo() override { m_controller->removeFrame(m_frame.id); }
    void redo() override { m_controller->insertFrameAt(m_pageIndex, m_frame); }

private:
    CanvasController *m_controller;
    Frame m_frame;
    int m_pageIndex;
};

void rotatePageTo(Page &page, bool targetLandscape);

class SetPaperCommand : public QUndoCommand
{
public:
    SetPaperCommand(CanvasController *controller, int pageIndex, PaperSpec paper,
                    QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_pageIndex(pageIndex)
    {
        if (pageIndex < 0 || pageIndex >= m_controller->document().pages.size())
            return;
        // The whole page on both sides, because a paper change re-maps the
        // slotted photographs proportionally with their cells. Storing the pages
        // makes undo exact instead of relying on the inverse of a scale.
        m_before = m_controller->document().pages.at(pageIndex);
        m_after = m_before;
        m_after.paper = std::move(paper);
        m_after = SlotGeometry::remapToPaper(m_before, std::move(m_after));
    }

    void undo() override
    {
        if (m_pageIndex >= 0)
            m_controller->replacePage(m_pageIndex, m_before);
    }
    void redo() override
    {
        if (m_pageIndex >= 0)
            m_controller->replacePage(m_pageIndex, m_after);
    }

private:
    CanvasController *m_controller;
    int m_pageIndex;
    Page m_before;
    Page m_after;
};

class RotatePageCommand : public QUndoCommand
{
public:
    RotatePageCommand(CanvasController *controller, int pageIndex, QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_pageIndex(pageIndex)
    {
        if (pageIndex < 0 || pageIndex >= m_controller->document().pages.size())
            return;
        m_before = m_controller->document().pages.at(pageIndex);
        m_after = m_before;
        rotatePageTo(m_after, !m_before.landscape);
    }

    void undo() override
    {
        if (m_pageIndex >= 0)
            m_controller->replacePage(m_pageIndex, m_before);
    }
    void redo() override
    {
        if (m_pageIndex >= 0)
            m_controller->replacePage(m_pageIndex, m_after);
    }

private:
    CanvasController *m_controller;
    int m_pageIndex;
    Page m_before;
    Page m_after;
};

void rotatePageTo(Page &page, bool targetLandscape)
{
    if (page.landscape == targetLandscape)
        return;
    // A slotted frame is re-mapped from the cell it had on the old sheet to the
    // cell it has on the new one, so its composition inside the section
    // survives the turn. The old page is the reference for that mapping.
    const Page before = page;
    const QSizeF oldSize = pageSizeMm(page);
    const bool forward = targetLandscape;
    page.landscape = targetLandscape;
    for (Frame &frame : page.frames) {
        if (!frame.slotId.isEmpty())
            continue;
        const double oldX = frame.cx;
        const double oldY = frame.cy;
        if (forward) {
            frame.cx = oldSize.height() - oldY;
            frame.cy = oldX;
            frame.angleDeg += 90.0;
        } else {
            frame.cx = oldY;
            frame.cy = oldSize.width() - oldX;
            frame.angleDeg -= 90.0;
        }
        while (frame.angleDeg > 180.0)
            frame.angleDeg -= 360.0;
        while (frame.angleDeg <= -180.0)
            frame.angleDeg += 360.0;
    }
    if (page.hasLayout())
        page = SlotGeometry::remapToPaper(before, std::move(page));
}

Page fittedPage(const Page &page)
{
    Page fitted = page;
    const QSizeF size = pageSizeMm(page);
    if (size.width() <= 0.0 || size.height() <= 0.0)
        return fitted;
    double factor = 1.0;
    for (const Frame &frame : page.frames) {
        // A slotted photograph is clipped to its cell, so it can never overflow
        // the sheet and fitting it would only break the composition inside the
        // section.
        if (!frame.slotId.isEmpty())
            continue;
        if (!LayoutEngine::validFrame(frame))
            continue;
        const double rad = qDegreesToRadians(frame.angleDeg);
        const double width =
            std::abs(frame.width * std::cos(rad)) + std::abs(frame.height * std::sin(rad));
        const double height =
            std::abs(frame.width * std::sin(rad)) + std::abs(frame.height * std::cos(rad));
        const double extentX = std::abs(frame.cx - size.width() / 2.0) + width / 2.0;
        const double extentY = std::abs(frame.cy - size.height() / 2.0) + height / 2.0;
        if (extentX > 0.0)
            factor = std::min(factor, (size.width() / 2.0) / extentX);
        if (extentY > 0.0)
            factor = std::min(factor, (size.height() / 2.0) / extentY);
    }
    factor = std::clamp(factor, 0.02, 1.0);
    for (Frame &frame : fitted.frames) {
        if (!frame.slotId.isEmpty())
            continue;
        frame.cx = size.width() / 2.0 + (frame.cx - size.width() / 2.0) * factor;
        frame.cy = size.height() / 2.0 + (frame.cy - size.height() / 2.0) * factor;
        frame.width *= factor;
        frame.height *= factor;
    }
    return fitted;
}

class FitPageCommand : public QUndoCommand
{
public:
    FitPageCommand(CanvasController *controller, int pageIndex, QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_pageIndex(pageIndex)
    {
        if (pageIndex >= 0 && pageIndex < m_controller->document().pages.size()) {
            m_before = m_controller->document().pages.at(pageIndex);
            m_after = fittedPage(m_before);
        }
    }

    void undo() override { m_controller->replacePage(m_pageIndex, m_before); }
    void redo() override { m_controller->replacePage(m_pageIndex, m_after); }

private:
    CanvasController *m_controller;
    int m_pageIndex;
    Page m_before;
    Page m_after;
};

class SnapshotCommand : public QUndoCommand
{
public:
    SnapshotCommand(CanvasController *controller, DocumentSnapshot before, DocumentSnapshot after,
                    const QString &text, QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_before(std::move(before)),
          m_after(std::move(after))
    {
        setText(text);
    }

    void undo() override { m_controller->replaceSnapshot(m_before); }
    void redo() override { m_controller->replaceSnapshot(m_after); }

private:
    CanvasController *m_controller;
    DocumentSnapshot m_before;
    DocumentSnapshot m_after;
};

class RemoveFrameCommand : public QUndoCommand
{
public:
    RemoveFrameCommand(CanvasController *controller, QString frameId,
                       QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_controller(controller), m_frameId(std::move(frameId))
    {
        const LocatedFrame located = locate(m_controller->document(), m_frameId);
        m_before = located.frame;
    }

    void undo() override { m_controller->insertFrame(m_before); }
    void redo() override { m_controller->removeFrame(m_frameId); }

private:
    CanvasController *m_controller;
    QString m_frameId;
    Frame m_before;
};

CanvasController::CanvasController(QObject *parent) : QObject(parent), m_undo(new QUndoStack(this))
{
    m_undo->setUndoLimit(200);
}

void CanvasController::setDocument(const DocumentSnapshot &document)
{
    m_document = document;
    repairDocument();
    m_undo->clear();
    emit documentChanged(m_document);
}

Result<void> CanvasController::apply(const DocumentEdit &edit)
{
    return std::visit(
        [this](const auto &value) -> Result<void> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, FrameChange>) {
                const LocatedFrame located = locate(m_document, value.frameId);
                if (located.frameIndex < 0)
                    return Result<void>();
                // Repair the proposed state before the no-op comparison, so a
                // FrameChange that repairs to the current document (a dangling
                // slot id, say) does not push a dead undo step. Geometry is not
                // repaired: a slotted frame's rectangle is the user's.
                DocumentSnapshot probe = m_document;
                if (Frame *target = findFrame(probe, value.frameId)) {
                    *target = value.frame;
                } else {
                    return Result<void>();
                }
                auto repaired = SlotLayoutEngine::repair(std::move(probe));
                if (!repaired)
                    return repaired.error();
                const Frame *after = findFrame(repaired.value(), value.frameId);
                if (!after || located.frame == *after)
                    return Result<void>();
                m_undo->push(new FrameChangeCommand(this, value.frameId, *after));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, RemoveFrameEdit>) {
                if (locate(m_document, value.frameId).frameIndex >= 0)
                    m_undo->push(new RemoveFrameCommand(this, value.frameId));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, AddFrameEdit>) {
                m_undo->push(new AddFrameCommand(this, value.frame, value.pageIndex));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, FitPageEdit>) {
                if (value.pageIndex >= 0 && value.pageIndex < m_document.pages.size()
                    && !m_document.pages.at(value.pageIndex).frames.isEmpty())
                    m_undo->push(new FitPageCommand(this, value.pageIndex));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, RotatePageEdit>) {
                if (value.pageIndex >= 0 && value.pageIndex < m_document.pages.size())
                    m_undo->push(new RotatePageCommand(this, value.pageIndex));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, AddPageEdit>) {
                DocumentSnapshot after = m_document;
                Page page;
                page.id = makeId(QStringLiteral("page"));
                if (after.currentPage >= 0 && after.currentPage < after.pages.size()) {
                    page.paper = after.pages.at(after.currentPage).paper;
                    page.landscape = after.pages.at(after.currentPage).landscape;
                }
                int index = value.afterIndex >= 0 && value.afterIndex < after.pages.size()
                    ? value.afterIndex + 1
                    : after.pages.size();
                after.pages.insert(index, page);
                after.currentPage = index;
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Lap hozzáadása")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, DuplicatePageEdit>) {
                if (value.pageIndex < 0 || value.pageIndex >= m_document.pages.size())
                    return Result<void>();
                DocumentSnapshot after = m_document;
                Page copy = after.pages.at(value.pageIndex);
                copy.id = makeId(QStringLiteral("page"));
                for (Frame &frame : copy.frames)
                    frame.id = makeId(QStringLiteral("frame"));
                after.pages.insert(value.pageIndex + 1, copy);
                after.currentPage = value.pageIndex + 1;
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Lap másolása")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, RemovePageEdit>) {
                if (m_document.pages.size() <= 1 || value.pageIndex < 0
                    || value.pageIndex >= m_document.pages.size())
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.pages.removeAt(value.pageIndex);
                after.currentPage = std::clamp(value.pageIndex, 0, int(after.pages.size()) - 1);
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Lap törlése")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, MovePageEdit>) {
                if (value.from < 0 || value.from >= m_document.pages.size() || value.to < 0
                    || value.to >= m_document.pages.size() || value.from == value.to)
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.pages.move(value.from, value.to);
                after.currentPage = value.to;
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Lap átrendezése")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, ReorderFrameEdit>) {
                const LocatedFrame located = locate(m_document, value.frameId);
                if (located.pageIndex < 0)
                    return Result<void>();
                Page &page = m_document.pages[located.pageIndex];
                const int target = std::clamp(located.frameIndex + value.delta, 0,
                                              int(page.frames.size()) - 1);
                if (target == located.frameIndex)
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.pages[located.pageIndex].frames.move(located.frameIndex, target);
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Rétegsorrend")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetProjectPaperEdit>) {
                bool differs = false;
                for (const Page &page : m_document.pages)
                    if (page.paper != value.paper)
                        differs = true;
                if (!differs)
                    return Result<void>();
                DocumentSnapshot after = m_document;
                for (Page &page : after.pages) {
                    // Each page is re-mapped from its own old sheet, so the
                    // composition inside every section survives the change.
                    const Page before = page;
                    page.paper = value.paper;
                    page = SlotGeometry::remapToPaper(before, std::move(page));
                }
                auto repaired = SlotLayoutEngine::repair(std::move(after));
                if (!repaired)
                    return repaired.error();
                m_undo->push(new SnapshotCommand(this, m_document, repaired.value(),
                                                 QCoreApplication::translate("PhotoPrint", "Papír minden lapon")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetProjectLandscapeEdit>) {
                DocumentSnapshot after = m_document;
                for (Page &page : after.pages)
                    rotatePageTo(page, value.landscape);
                if (after == m_document)
                    return Result<void>();
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Tájolás minden lapon")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetPrintQualityEdit>) {
                if (m_document.fineQuality == value.fineQuality)
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.fineQuality = value.fineQuality;
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Nyomtatási minőség")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetRecipeEdit>) {
                if (m_document.recipe == value.recipe)
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.recipe = value.recipe;
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Színbeállítás")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetFramesEdit>) {
                if (value.pageIndex < 0 || value.pageIndex >= m_document.pages.size())
                    return Result<void>();
                DocumentSnapshot after = m_document;
                after.pages[value.pageIndex].frames = value.frames;
                // Repair the slot assignments first; if the result equals the
                // current document it is not an edit and must not push a dead
                // undo step. Frame geometry is the user's and is kept.
                auto repaired = SlotLayoutEngine::repair(std::move(after));
                if (!repaired)
                    return repaired.error();
                after = repaired.value();
                if (after == m_document)
                    return Result<void>();
                m_undo->push(new SnapshotCommand(this, m_document, after,
                                                 QCoreApplication::translate("PhotoPrint", "Elrendezés")));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, ApplyLayoutEdit>) {
                auto valid = validateLayoutEdit(m_document, value);
                if (!valid)
                    return valid;
                DocumentSnapshot after = m_document;
                int firstChanged = -1;
                for (const PageLayout &change : value.pages) {
                    if (change.pageIndex < 0 || change.pageIndex >= after.pages.size())
                        continue;
                    Page &page = after.pages[change.pageIndex];
                    if (change.frames)
                        page.frames = *change.frames;
                    if (change.cutGuides)
                        page.cutGuides = *change.cutGuides;
                    if (change.borderlessOverride)
                        page.borderlessOverride = *change.borderlessOverride;
                    if (change.slotList)
                        page.slotList = *change.slotList;
                    if (change.slotStyle)
                        page.slotStyle = *change.slotStyle;
                    if (change.layoutId)
                        page.layoutId = *change.layoutId;
                    if (firstChanged < 0 || change.pageIndex < firstChanged)
                        firstChanged = change.pageIndex;
                }
                const int insertAt = value.insertIndex < 0
                    ? int(after.pages.size())
                    : std::clamp(value.insertIndex, 0, int(after.pages.size()));
                for (int i = 0; i < value.newPages.size(); ++i)
                    after.pages.insert(insertAt + i, value.newPages.at(i));

                // Repair before the no-op comparison. A stale-geometry edit that
                // repairs to the current document is navigation, not an edit.
                auto repaired = SlotLayoutEngine::repair(std::move(after));
                if (!repaired)
                    return repaired.error();
                after = repaired.value();

                // A page selection alone is navigation, not an edit: it changes
                // currentPage and emits the document, but adds no undo step.
                if (after == m_document) {
                    if (value.selectPage >= 0 && value.selectPage != m_document.currentPage)
                        setCurrentPage(value.selectPage);
                    return Result<void>();
                }

                int target = value.selectPage;
                if (target < 0) {
                    if (firstChanged >= 0) {
                        target = firstChanged;
                        if (!value.newPages.isEmpty() && insertAt <= target)
                            target += int(value.newPages.size());
                    } else if (!value.newPages.isEmpty()) {
                        target = insertAt;
                    } else {
                        target = after.currentPage;
                    }
                }
                after.currentPage = std::clamp(target, 0, int(after.pages.size()) - 1);

                const QString text = value.actionText.isEmpty()
                    ? QCoreApplication::translate("PhotoPrint", "Elrendezés")
                    : value.actionText;
                m_undo->push(new SnapshotCommand(this, m_document, after, text));
                return Result<void>();
            } else if constexpr (std::is_same_v<T, SetPaperEdit>) {
                if (value.pageIndex >= 0 && value.pageIndex < m_document.pages.size()
                    && m_document.pages.at(value.pageIndex).paper != value.paper)
                    m_undo->push(new SetPaperCommand(this, value.pageIndex, value.paper));
                return Result<void>();
            }
            return Result<void>();
        },
        edit);
}

Result<void> CanvasController::publishPage(const Page &page, const QString &actionText)
{
    if (page.id.isEmpty()) {
        return error(QStringLiteral("page-missing"),
                     QCoreApplication::translate("PhotoPrint", "Érvénytelen lapsorszám."));
    }
    const int index = pageIndexOf(m_document, page.id);
    if (index < 0) {
        return error(QStringLiteral("page-missing"),
                     QCoreApplication::translate("PhotoPrint", "Érvénytelen lapsorszám."));
    }
    // Every field of the finished page is published, so a caller cannot forget
    // the slot list, the style or the label: the page it computed is the page
    // that lands, and a validation failure comes back to the caller.
    ApplyLayoutEdit edit;
    PageLayout change;
    change.pageIndex = index;
    change.frames = page.frames;
    change.cutGuides = page.cutGuides;
    change.borderlessOverride = page.borderlessOverride;
    change.slotList = page.slotList;
    change.slotStyle = page.slotStyle;
    change.layoutId = page.layoutId;
    edit.pages.append(change);
    edit.actionText = actionText;
    edit.selectPage = index;
    return apply(edit);
}

void CanvasController::applyFrame(const QString &frameId, const Frame &frame)
{
    Frame *target = findFrame(m_document, frameId);
    if (!target)
        return;
    *target = frame;
    repairDocument();
    emit documentChanged(m_document);
}

void CanvasController::removeFrame(const QString &frameId)
{
    for (int p = 0; p < m_document.pages.size(); ++p) {
        Page &page = m_document.pages[p];
        for (int f = 0; f < page.frames.size(); ++f) {
            if (page.frames.at(f).id == frameId) {
                page.frames.removeAt(f);
                repairDocument();
                emit documentChanged(m_document);
                return;
            }
        }
    }
}

void CanvasController::insertFrame(const Frame &frame)
{
    insertFrameAt(-1, frame);
}

void CanvasController::setCurrentPage(int index)
{
    if (index < 0 || index >= m_document.pages.size() || index == m_document.currentPage)
        return;
    m_document.currentPage = index;
    emit documentChanged(m_document);
}

void CanvasController::replaceSnapshot(const DocumentSnapshot &snapshot)
{
    m_document = snapshot;
    if (m_document.currentPage < 0 || m_document.currentPage >= m_document.pages.size())
        m_document.currentPage = std::clamp(m_document.currentPage, 0,
                                            std::max(0, int(m_document.pages.size()) - 1));
    // Undo and redo land here. Repairing keeps the slot invariant (at most one
    // frame per slot, no dangling id) without touching the frame geometry a
    // person composed inside a cell.
    repairDocument();
    emit documentChanged(m_document);
}

void CanvasController::replacePage(int pageIndex, const Page &page)
{
    if (pageIndex < 0 || pageIndex >= m_document.pages.size())
        return;
    m_document.pages[pageIndex] = page;
    repairDocument();
    emit documentChanged(m_document);
}

void CanvasController::fitPageToPaper(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_document.pages.size())
        return;
    const Page fitted = fittedPage(m_document.pages.at(pageIndex));
    replacePage(pageIndex, fitted);
}

void CanvasController::insertFrameAt(int pageIndex, const Frame &frame)
{
    if (m_document.pages.isEmpty())
        return;
    const int index = pageIndex >= 0 && pageIndex < m_document.pages.size()
        ? pageIndex
        : m_document.currentPage;
    m_document.pages[index].frames.append(frame);
    repairDocument();
    emit documentChanged(m_document);
}

void CanvasController::repairDocument()
{
    const auto repaired = SlotLayoutEngine::repair(m_document);
    if (!repaired)
        return;
    m_document = repaired.value();
#ifndef QT_NO_DEBUG
    // The invariant is asserted here and tested directly; a debug build stops at
    // the first writer that broke it instead of painting a drifted page.
    for (const Page &page : m_document.pages) {
        const QSizeF sheet = pageSizeMm(page);
        if (sheet.width() > 0.0 && sheet.height() > 0.0)
            Q_ASSERT(SlotGeometry::layoutConsistent(page));
    }
#endif
}

} // namespace pp
