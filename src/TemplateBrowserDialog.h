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

namespace pp {

/// Everything the picker needs from the page it describes: the paper the slots
/// are mapped onto, the frames whose count and order the preview shows, and one
/// small thumbnail per frame so a slot can preview the photograph that will land
/// in it. The dialog keeps its own copy and never touches the document.
struct TemplateBrowserRequest {
    PaperSpec paper;
    bool landscape = false;
    QList<Frame> frames;  ///< the page's frames, in fill order
    /// Optional, one entry per frame (a null image is fine). Slot i shows
    /// photos.at(i), which is the order a template fills.
    QList<QImage> photos;
    /// Logical width of one template preview. The page aspect decides the
    /// height, and the raster is capped at kMaxPreviewPx device pixels.
    int tileLogicalPx = 120;
};

/// One tile of the current count. The preview is rebuilt when the count or the
/// page changes; the check mark is painted into it on selection. It lives at
/// namespace scope because moc cannot describe a nested class of a QObject.
struct TemplateTileState {
    QString id;
    QString nameEn;
    int photoCount = 0;
    bool mapped = false; ///< slotsMm() succeeded on this paper
    QString paperMessage;
    QList<QRectF> slotRects; ///< page millimetres, from slotsMm()
    QPixmap preview;     ///< page-aspect preview without the check
    QPixmap marked;      ///< with the check, built on first use
};

/// Collage template picker for the current page.
///
/// The dialog is a preview and a chooser: it maps every browsable template
/// against the page's paper only to draw it and to tell the truth about what
/// apply() would do (the crop reset warning), but it never edits the document.
/// Apply emits templateChosen() with the id and closes; MainWindow publishes the
/// single SetFramesEdit. Browsing is never disabled, so somebody with an empty
/// page still finds the feature and is told how many photos each layout needs.
class TemplateBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TemplateBrowserDialog(const TemplateBrowserRequest &request,
                                   QWidget *parent = nullptr);

    /// Follows the page while the dialog is open, so adding photographs from the
    /// dialog's own button turns the mismatch line into an enabled Apply.
    void setPage(const PaperSpec &paper, bool landscape, const QList<Frame> &frames,
                 const QList<QImage> &photos = {});

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
    QString templateLabel(const QString &id) const;          ///< the English name
    QString templateAccessibleName(const QString &id) const;
    QImage templateTileImage(const QString &id) const;       ///< as painted
    bool templateMarkedChosen(const QString &id) const;      ///< the check mark
    bool templateEnabled(const QString &id) const;           ///< false when too small

    bool applyEnabled() const;
    QString countMismatchText() const; ///< visible mismatch line, or empty
    QString capacityText() const;      ///< the 11+ line, or empty
    QString paperMessageText() const;  ///< template-too-small message, or empty
    QString cropWarningText() const;   ///< the one crop reset line, or empty
    bool addPhotosVisible() const;

signals:
    /// The chosen template, on Apply. The dialog does not apply it.
    void templateChosen(const QString &templateId);
    /// Add photographs to the current page. The caller owns the file dialog.
    void addPhotosRequested();

private:
    void buildUi();
    void fillChips();
    void rebuildTiles();
    void refreshSelection();
    void refreshStatus();
    void chooseSelected();
    /// Moves the count selection by @p delta from the keyboard and keeps the
    /// focus on the chip that is now checked.
    void stepCount(int delta);
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
    bool m_building = false;

    QLabel *m_subtitle = nullptr;
    QWidget *m_chipRow = nullptr;
    QHBoxLayout *m_chipLayout = nullptr;
    QButtonGroup *m_chipGroup = nullptr;
    QList<QPushButton *> m_chipButtons;
    QListWidget *m_grid = nullptr;
    QLabel *m_mismatchLabel = nullptr;
    QLabel *m_capacityLabel = nullptr;
    QLabel *m_paperLabel = nullptr;
    QLabel *m_cropLabel = nullptr;
    QPushButton *m_addPhotosButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};

} // namespace pp
