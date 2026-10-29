#pragma once

#include "Document.h"

#include <QObject>
#include <QUndoStack>

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
                                  SetRecipeEdit, SetProjectPaperEdit, SetProjectLandscapeEdit,
                                  SetPrintQualityEdit>;

/// Owns the edited document and its undo history. Every user-visible edit goes
/// through here; a long drag is committed once, not per mouse move.
class CanvasController : public QObject
{
    Q_OBJECT

public:
    explicit CanvasController(QObject *parent = nullptr);

    DocumentSnapshot document() const { return m_document; }
    void setDocument(const DocumentSnapshot &document);
    void setCurrentPage(int index);
    void apply(const DocumentEdit &edit);

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

    DocumentSnapshot m_document;
    QUndoStack *m_undo = nullptr;
};

} // namespace pp
