#pragma once

#include "CollageTemplates.h"
#include "Document.h"

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSizeF>
#include <QStringList>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace pp {

/// Everything the picker needs from the page it describes: the paper the slots
/// are mapped onto, the frames whose count and order the preview shows, and one
/// small thumbnail per frame so a slot can preview the photograph that will land
/// in it. The dialog keeps its own copy and never touches the document.
struct TemplateBrowserRequest {
    PaperSpec paper;
    bool landscape = false;
    QList<Frame> frames;  ///< the page's frames; reordered to display order on intake
    /// The page's slots, so a slotted page maps its photographs to the right
    /// preview cells instead of to their z-order.
    QList<Slot> slotList;
    /// The page's style and layout label. The label names the layout the page
    /// already uses, so the picker can mark it and applying it is a no-op.
    SlotStyle slotStyle;
    QString layoutId;
    /// Optional, one entry per frame (a null image is fine). Slot i shows
    /// photos.at(i) after both lists are reordered into display order.
    QList<QImage> photos;
    /// Logical width of one template preview. The page aspect decides the
    /// height, and the raster is capped at kMaxPreviewPx device pixels.
    int tileLogicalPx = 120;
};

/// One tile of the current count. The preview is rebuilt when the count, the
/// paper, the photographs or the current layout change; the check mark and the
/// current-layout ring are painted into it on selection. It lives at namespace
/// scope because moc cannot describe a nested class of a QObject.
struct TemplateTileState {
    QString id;
    QString nameEn;
    int photoCount = 0;
    bool mapped = false; ///< slotsMm() succeeded on this paper
    QString paperMessage;
    QList<QRectF> slotRects; ///< page millimetres, from slotsMm()
    QPixmap preview;         ///< page-aspect preview without a mark
    QPixmap marked;          ///< with the selection check, built on first use
    QPixmap current;         ///< with the current-layout ring
    QPixmap markedCurrent;   ///< with both
};

/// Collage template picker for the current page.
///
/// The dialog is a preview and a chooser: it maps every browsable layout
/// against the page's paper to draw it, and it runs the slot engine on a copy
/// only to tell the truth about what applying would change. It never edits the
/// document. A count is a filter, never a gate: a layout may be applied to a
/// page with fewer photographs (the rest stay empty) or more (the surplus stays
/// free), and the page's own layout is marked and named when it has one.
///
/// Apply emits templateChosen() and nothing else: MainWindow publishes the one
/// page edit, closes the dialog on success, and hands a refusal back through
/// reportApplyFailure() so the picker stays open with the reason on screen.
class TemplateBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TemplateBrowserDialog(const TemplateBrowserRequest &request,
                                   QWidget *parent = nullptr);

    /// Follows the page while the dialog is open. The chip follows the page's
    /// layout when it has one and its photo count otherwise, and the tiles and
    /// the guidance are updated once the burst of edits is over, so a nudge does
    /// not rebuild six previews.
    void setPage(const PaperSpec &paper, bool landscape, const QList<Frame> &frames,
                 const QList<QImage> &photos = {}, const QList<Slot> &slotList = {},
                 const SlotStyle &slotStyle = {}, const QString &layoutId = QString());
    /// The reason the last Apply was refused, shown inside the dialog.
    void reportApplyFailure(const QString &message);
    /// Processes events until a scheduled page sync has run.
    bool waitForPendingSync(int timeoutMs = 2000);

    // Inspection hooks for the tests. They describe what the user can see.
    int countChipCount() const;
    int countChipValue(int index) const;
    bool countChipEnabled(int count) const;
    void selectCount(int count);
    int selectedCount() const;

    int templateTileCount() const;
    QStringList templateIds() const; ///< visible tiles, in catalogue order
    void selectTemplate(const QString &id);
    QString selectedTemplateId() const;
    QString templateLabel(const QString &id) const;   ///< the English name
    QString templateAccessibleName(const QString &id) const;
    QString templateAccessibleDescription(const QString &id) const;
    QImage templateTileImage(const QString &id) const; ///< as painted
    bool templateMarkedChosen(const QString &id) const; ///< the check mark
    bool templateMapped(const QString &id) const;       ///< slotsMm() on this paper
    /// The layout the page currently uses, and whether a tile is that layout.
    QString currentLayoutId() const { return m_request.layoutId; }
    bool templateIsCurrent(const QString &id) const;
    QString currentLayoutText() const; ///< the visible current-layout line, or empty
    QString gridAccessibleName() const;
    /// How many times the tiles were (re)built. A burst of document changes
    /// must end in one rebuild, and a nudge in none.
    int tileRebuildCount() const { return m_rebuilds; }

    bool applyEnabled() const;
    QString paperMessageText() const;   ///< template-too-small message, or empty
    QString positionNoticeText() const; ///< what Apply changes, or empty
    QString applyErrorText() const;     ///< a refusal from the last Apply

signals:
    /// The chosen template, on Apply. The dialog does not apply it.
    void templateChosen(const QString &templateId);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void fillChips();
    void rebuildTiles();
    void refreshSelection();
    void refreshStatus();
    void chooseSelected();
    /// Reorders `frames` and their parallel `photos` into the page's display
    /// order (slotted photographs by slot index, then free ones in z-order).
    /// Slot i is then the i-th photograph a template fills, which is what the
    /// preview draws.
    void orderByDisplay();
    /// Moves the count selection by @p delta from the keyboard and keeps the
    /// focus on the chip that is now checked.
    void stepCount(int delta);
    void scheduleSync();
    void runPendingSync();
    QString signature() const;
    void announce();
    QSizeF pageMm() const;
    QSize previewSize() const;
    int currentChipCount() const;
    /// The chip the page asks for: the current layout's count when it names a
    /// catalogue layout, otherwise the page's photo count. Clamped to the chips.
    int preferredCount() const;
    /// The page as the picker's engine copy sees it, with the current slots,
    /// style and layout label, so a top-up prediction is the real one.
    Page requestPage() const;
    int indexOfTemplate(const QString &id) const;
    const CollageTemplate *templateFor(const QString &id) const;
    QPixmap renderPreview(const QList<QRectF> &slotRects, bool marked, bool current) const;
    /// The one sentence Apply would produce from the engine's own report.
    QString positionNotice(const TemplateTileState &tile) const;
    QString countLabel(int count) const;

    TemplateBrowserRequest m_request;
    QList<TemplateTileState> m_tiles; ///< tiles of the selected count, in catalogue order
    QString m_selectedId;
    QString m_applyError;
    QString m_builtSignature;
    QString m_announced;
    int m_rebuilds = 0;
    bool m_building = false;

    QLabel *m_subtitle = nullptr;
    QLabel *m_currentLabel = nullptr;
    QWidget *m_chipRow = nullptr;
    QHBoxLayout *m_chipLayout = nullptr;
    QButtonGroup *m_chipGroup = nullptr;
    QList<QPushButton *> m_chipButtons;
    QListWidget *m_grid = nullptr;
    QLabel *m_paperLabel = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTimer *m_syncTimer = nullptr;
};

} // namespace pp
