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
    /// Optional, one entry per frame (a null image is fine). Slot i shows
    /// photos.at(i) after both lists are reordered into display order.
    QList<QImage> photos;
    /// Logical width of one template preview. The page aspect decides the
    /// height, and the raster is capped at kMaxPreviewPx device pixels.
    int tileLogicalPx = 120;
};

/// One tile of the current count. The preview is rebuilt when the count, the
/// paper or the photographs change; the check mark is painted into it on
/// selection. It lives at namespace scope because moc cannot describe a nested
/// class of a QObject.
struct TemplateTileState {
    QString id;
    QString nameEn;
    int photoCount = 0;
    bool mapped = false; ///< slotsMm() succeeded on this paper
    QString paperMessage;
    QList<QRectF> slotRects; ///< page millimetres, from slotsMm()
    QPixmap preview;         ///< page-aspect preview without the check
    QPixmap marked;          ///< with the check, built on first use
};

/// Collage template picker for the current page.
///
/// The dialog is a preview and a chooser: it maps every browsable layout
/// against the page's paper to draw it, and it runs apply() on a copy only to
/// tell the truth about what applying would change. It never edits the
/// document and it never claims the page already uses a layout, because a
/// project stores frames and no template id.
///
/// Apply emits templateChosen() and nothing else: MainWindow publishes the one
/// SetFramesEdit, closes the dialog on success, and hands a refusal back through
/// reportApplyFailure() so the picker stays open with the reason on screen.
class TemplateBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TemplateBrowserDialog(const TemplateBrowserRequest &request,
                                   QWidget *parent = nullptr);

    /// Follows the page while the dialog is open. The count chip follows the
    /// page's photo count, and the tiles and the guidance are updated once the
    /// burst of edits is over, so a nudge does not rebuild six previews.
    void setPage(const PaperSpec &paper, bool landscape, const QList<Frame> &frames,
                 const QList<QImage> &photos = {}, const QList<Slot> &slotList = {});
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
    QString gridAccessibleName() const;
    /// How many times the tiles were (re)built. A burst of document changes
    /// must end in one rebuild, and a nudge in none.
    int tileRebuildCount() const { return m_rebuilds; }

    bool applyEnabled() const;
    QString countMismatchText() const;  ///< visible count line, or empty
    QString directionHintText() const;  ///< what to do with too many photos
    QString capacityText() const;       ///< the 11+ line, or empty
    QString paperMessageText() const;   ///< template-too-small message, or empty
    QString positionNoticeText() const; ///< what Apply changes, or empty
    QString applyErrorText() const;     ///< a refusal from the last Apply
    bool addPhotosVisible() const;

signals:
    /// The chosen template, on Apply. The dialog does not apply it.
    void templateChosen(const QString &templateId);
    /// Add photographs to the current page. The caller owns the file dialog.
    void addPhotosRequested();

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
    int indexOfTemplate(const QString &id) const;
    const CollageTemplate *templateFor(const QString &id) const;
    QPixmap renderPreview(const QList<QRectF> &slotRects, bool marked) const;
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
    QWidget *m_chipRow = nullptr;
    QHBoxLayout *m_chipLayout = nullptr;
    QButtonGroup *m_chipGroup = nullptr;
    QList<QPushButton *> m_chipButtons;
    QListWidget *m_grid = nullptr;
    QLabel *m_mismatchLabel = nullptr;
    QLabel *m_directionLabel = nullptr;
    QLabel *m_capacityLabel = nullptr;
    QLabel *m_paperLabel = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QPushButton *m_addPhotosButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTimer *m_syncTimer = nullptr;
};

} // namespace pp
