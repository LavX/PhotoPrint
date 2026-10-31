#pragma once

#include "Document.h"
#include "Result.h"

#include <QObject>
#include <QUndoStack>

#include <optional>
#include <variant>

namespace pp {

/// One complete replacement of a frame's geometry and inner image state.
struct FrameChange {
    QString frameId;
    Frame frame;
};

struct RemoveFrameEdit {
    QString frameId;
};

struct AddFrameEdit {
    Frame frame;
    int pageIndex = -1; ///< -1 means the current page
};

struct SetPaperEdit {
    int pageIndex = 0;
    PaperSpec paper;
};

struct RotatePageEdit {
    int pageIndex = 0;
};

struct FitPageEdit {
    int pageIndex = 0;
};

struct AddPageEdit {
    int afterIndex = -1;
};

struct DuplicatePageEdit {
    int pageIndex = 0;
};

struct RemovePageEdit {
    int pageIndex = 0;
};

struct MovePageEdit {
    int from = 0;
    int to = 0;
};

struct ReorderFrameEdit {
    QString frameId;
    int delta = 1; ///< +1 towards the top, -1 towards the bottom
};

struct SetFramesEdit {
    int pageIndex = 0;
    QList<Frame> frames;
};

/// One layout action's change to one existing page. Every field is optional:
/// a guide-only edit must not have to restate the frames, and an align edit
/// must not silently reset the page's cut guide state.
struct PageLayout {
    int pageIndex = -1;
    std::optional<QList<Frame>> frames;
    std::optional<CutGuides> cutGuides;
    std::optional<BorderlessOverride> borderlessOverride;
    std::optional<QList<Slot>> slotList;
    std::optional<SlotStyle> slotStyle;
    std::optional<QString> layoutId;
};

/// A batch or align action, applied as one snapshot. The caller supplies the
/// finished pages; the controller never computes geometry. `insertIndex` is
/// where the new page block goes (-1 appends), and `selectPage` is the page
/// left current (-1 picks the first touched page). `actionText` is the undo
/// text; empty falls back to "Elrendezés".
struct ApplyLayoutEdit {
    QList<PageLayout> pages;
    QList<Page> newPages;
    int insertIndex = -1;
    int selectPage = -1;
    QString actionText;
};

/// Checks an ApplyLayoutEdit against the document it will be applied to. The
/// controller refuses a malformed edit as a whole, never half applies it: an
/// unknown page index, a page changed twice, an empty or duplicate new page id,
/// an insertion index outside the page list, and a selected page that will not
/// exist. The dialog can call this before it commits its own state.
Result<void> validateLayoutEdit(const DocumentSnapshot &document, const ApplyLayoutEdit &edit);

struct SetRecipeEdit {
    ColorRecipe recipe;
};

struct SetProjectPaperEdit {
    PaperSpec paper;
};

struct SetProjectLandscapeEdit {
    bool landscape = false;
};

struct SetPrintQualityEdit {
    bool fineQuality = true;
};

using DocumentEdit = std::variant<FrameChange, RemoveFrameEdit, AddFrameEdit, SetPaperEdit,
                                  RotatePageEdit, FitPageEdit, AddPageEdit, DuplicatePageEdit,
                                  RemovePageEdit, MovePageEdit, ReorderFrameEdit, SetFramesEdit,
                                  ApplyLayoutEdit, SetRecipeEdit, SetProjectPaperEdit,
                                  SetProjectLandscapeEdit, SetPrintQualityEdit>;

/// Owns the edited document and its undo history. Every user-visible edit goes
/// through here; a long drag is committed once, not per mouse move.
class CanvasController : public QObject
{
    Q_OBJECT

public:
    explicit CanvasController(QObject *parent = nullptr);

    DocumentSnapshot document() const { return m_document; }
    int pageCount() const { return static_cast<int>(m_document.pages.size()); }
    void setDocument(const DocumentSnapshot &document);
    void setCurrentPage(int index);
    /// Applies one edit. Returns an error when the edit is malformed (a bad page
    /// index, a slot list that disagrees with its frames, an invalid style), so
    /// a caller never has to guess whether an edit landed. A guard that makes the
    /// edit a no-op returns success without touching the undo stack.
    Result<void> apply(const DocumentEdit &edit);

    /// Publishes one finished page as a single undo step. The whole page is
    /// supplied, so the frames, the slot list, the style and the layout label
    /// cannot drift apart and a forgotten field cannot silently refuse the edit.
    /// Returns the validation error instead of dropping it.
    Result<void> publishPage(const Page &page,
                             const QString &actionText = QString());

    QUndoStack *undoStack() const { return m_undo; }
    bool canUndo() const { return m_undo->canUndo(); }
    bool canRedo() const { return m_undo->canRedo(); }

signals:
    void documentChanged(const DocumentSnapshot &document);

private:
    friend class FrameChangeCommand;
    friend class RemoveFrameCommand;
    friend class AddFrameCommand;
    friend class SetPaperCommand;
    friend class RotatePageCommand;
    friend class FitPageCommand;
    friend class SnapshotCommand;
    friend class ReorderFrameCommand;
    void applyFrame(const QString &frameId, const Frame &frame);
    void removeFrame(const QString &frameId);
    void insertFrame(const Frame &frame);
    void insertFrameAt(int pageIndex, const Frame &frame);
    void setPaper(int pageIndex, const PaperSpec &paper);
    void setPageLandscape(int pageIndex, bool landscape);
    void fitPageToPaper(int pageIndex);
    void replacePage(int pageIndex, const Page &page);
    void replaceSnapshot(const DocumentSnapshot &snapshot);
    /// The single repair point. Every mutation below calls it before emitting,
    /// so no other writer (an align, a paper setter, an undo) can leave a slotted
    /// frame's geometry out of step with its slot.
    void repairDocument();

    DocumentSnapshot m_document;
    QUndoStack *m_undo = nullptr;
};

} // namespace pp
