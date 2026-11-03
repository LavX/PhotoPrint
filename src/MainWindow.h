#pragma once

#include "CanvasController.h"
#include "Document.h"
#include "ImageSource.h"
#include "PageRenderer.h"
#include "CupsBackend.h"
#include "JobController.h"
#include "PaperCatalog.h"
#include "PaperPresetStore.h"
#include "PagesPanel.h"
#include "PrinterCatalog.h"
#include "ProjectStore.h"
#include "RecoveryStore.h"

#include <QHash>
#include <QElapsedTimer>
#include <QImage>
#include <QListWidget>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>

#include <QFutureWatcher>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QFontComboBox;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class QToolButton;

namespace pp {

class EffectBrowserDialog;
class SheetToolsDialog;
class TemplateBrowserDialog;

/// The page's borderless decision. `Auto` follows the printer's own default;
/// `Off` and `On` pin it per page, so a saved batch sheet prints at its exact
/// copy size even when the printer would offer a bleed. `On` is still checked
/// against what the printer accepts further down the print path.
bool resolvedBorderless(const Page &page, bool printerBorderless);

/// The drag payload a photo in the library publishes. The frame id travels as
/// the data, so the canvas knows exactly which photo to move instead of guessing
/// from a thumbnail. Never shown to the user.
inline constexpr char kFrameMimeType[] = "application/x-photoprint-frame";

/// The left photo list. It is the current page's frames, so dragging out of it
/// is a move, never a copy: the canvas applies the frame id to the slot it is
/// dropped on. Dragging is the only drag mode; the list keeps its own order.
class PhotoListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit PhotoListWidget(QWidget *parent = nullptr);

protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override;
};

/// The paper surface. It paints the same page recipe the export uses and adds
/// editor-only marks; the marks never reach the PDF.
class PageView : public QWidget
{
    Q_OBJECT

public:
    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;

    void setDocument(const DocumentSnapshot &document);
    void setPageImages(const PageImages &images);
    /// Test-only convenience: the frames of a page with no background picture.
    void setFrameImages(const QHash<QString, QImage> &images);
    void setViewZoom(double zoom);
    double viewZoom() const { return m_viewZoom; }
    void setSelectedFrame(const QString &frameId);
    QString selectedFrame() const { return m_selectedId; }
    /// Which slot the selection sits in. A selected photo inside a layout also
    /// selects its cell, so the inspector can talk about the cell even before
    /// the layout panel exists. An empty id means no cell is selected.
    void setSelectedSlot(const QString &slotId);
    QString selectedSlot() const { return m_selectedSlotId; }
    /// The page's overlays. Stickers and texts sit above the photographs and
    /// share one `z` stack, so a click on the topmost one selects it; the Fotók
    /// list is the way back to the photograph. A frame, a cell, a sticker and a
    /// text are four exclusive selections.
    void setSelectedSticker(const QString &stickerId);
    QString selectedSticker() const { return m_selectedStickerId; }
    void setSelectedText(const QString &textId);
    QString selectedText() const { return m_selectedTextId; }
    /// The stickers the window proved missing. The canvas marks each with its
    /// own placeholder; the page raster never does.
    void setMissingStickers(const QSet<QString> &stickerIds);
    /// A sticker's rotated box in widget coordinates, empty when it is not on
    /// the current page. Shared by the paint path, the hit test and the tests,
    /// so the outline a person sees is the outline that answers the pointer.
    QPolygonF stickerQuad(const QString &stickerId) const;
    /// A text's rotated block in widget coordinates, empty when it is not on the
    /// current page. The block is measured at the fixed reference ppi, so the
    /// outline does not move when the view zooms.
    QPolygonF textQuad(const QString &textId) const;
    /// The expanded ring the text handles sit on, in widget coordinates. It is
    /// outside the block, so a short caption is never covered by its own
    /// furniture. Shared by the paint path, the hit test and the tests.
    QPolygonF textHandleQuad(const QString &textId) const;
    /// Shows a sticker without committing it. The opacity slider uses it while
    /// it is being dragged; the window commits one edit when the drag ends.
    void previewSticker(const Sticker &sticker);
    std::optional<Sticker> liveSticker(const QString &stickerId) const;
    /// The same live-preview seam for a text block.
    void previewText(const TextItem &text);
    std::optional<TextItem> liveText(const QString &textId) const;
    /// The page-wide hint line, empty when the page has content. A test hook:
    /// the hint must not cover a sticker on an otherwise empty page.
    QString emptyHint() const { return m_emptyHint; }
    /// The frame as the canvas currently shows it, including an uncommitted
    /// wheel burst. The inspector builds its next action on this.
    std::optional<Frame> liveFrame(const QString &frameId) const;
    /// Shows a frame without committing it. The inspector uses it while a
    /// slider is dragged; the window commits one edit when the drag ends.
    void previewFrame(const Frame &frame);
    /// Lands a pending wheel burst now as one edit. An inspector action calls
    /// this first, so a wheel gesture and the action stay separate undo steps.
    void flushWheelBurst();
    /// Lands a pending arrow-key nudge now as one edit, for the same reason: a
    /// panel action inside the burst must not publish a stale position.
    void flushStickerBurst();
    /// The same for a text block's nudge burst. The two bursts have their own
    /// timers, so one kind's pending move can never swallow the other's.
    void flushTextBurst();

    /// How a laid-out cell looks on screen right now. The three placeholder
    /// states are screen-only; they are never part of the page raster.
    enum class SlotState {
        Filled,  ///< a photograph is decoded and the raster shows it
        Empty,   ///< the cell has no photograph at all
        Loading, ///< a photograph is assigned but its pixels are still decoding
        Missing  ///< the photograph cannot be read (missing or undecodable)
    };
    SlotState slotState(const QString &slotId) const;
    /// The frame ids the window has proven unreadable. A cell is "loading" until
    /// the window says otherwise, so a fresh drop never flashes "not found".
    void setUnavailableFrames(const QSet<QString> &frameIds);
    /// A slot's cell in widget coordinates, or an empty rect. A utility for
    /// tests and for pointing a hint at a cell; it is not used by paintEvent.
    QRectF slotRect(const QString &slotId) const;
    /// The slot under a widget point, or an empty id.
    QString slotAtWidget(const QPointF &widgetPoint) const;
    /// While a drag hovers a filled cell this is the cue the canvas shows and
    /// announces (`Csere`); empty otherwise. Test hook and accessibility aid.
    QString dropCue() const { return m_dropCue; }
    /// Test hooks: run the platform's drag handlers without a live drag
    /// manager, which the offscreen platform does not have. The handlers are
    /// the same code the window sees in use.
    void dragEnterForTest(QDragEnterEvent *event) { dragEnterEvent(event); }
    void dragMoveForTest(QDragMoveEvent *event) { dragMoveEvent(event); }
    void dragLeaveForTest(QDragLeaveEvent *event) { dragLeaveEvent(event); }
    void dropForTest(QDropEvent *event) { dropEvent(event); }
    void setCropMode(bool enabled);
    bool cropMode() const { return m_cropMode; }
    void setEmptyHint(const QString &text);
    void setThemeColors(const QColor &canvas, const QColor &border, const QColor &shadow);
    /// A small non-blocking overlay in the corner while a preview generation
    /// runs. The cancel button calls onCancelBusy; the widget never covers the
    /// rest of the canvas.
    void setBusyOverlay(bool busy);
    bool busyOverlayVisible() const;

    QSizeF pageMm() const;
    QRectF pageRect() const;
    /// Blocks until the raster matches the current document, so callers can read
    /// pixels. The paint path never uses this; it draws what is installed.
    QImage composedImage() const;
    /// The size of the raster installed right now, without waiting for a build.
    QSize composedRasterSize() const { return m_composed.size(); }

    /// Test hook: how many page rasters have been installed. Panning never
    /// raises it; a zoom or a resize raises it once, when the settle fires.
    quint64 compositionCount() const { return m_compositions; }
    /// Test hook: how many raster builds were started, installed or not.
    quint64 compositionRequestCount() const { return m_composeRequests; }
    /// Test hook: is a build running on the worker thread right now.
    bool compositionPending() const;
    /// Test hook: make the following builds fail the way an allocation or an
    /// lcms error would, so the retry guard can be exercised.
    void failCompositionsForTest(bool fail) { m_failCompositions = fail; }
    /// Test hook: keep the worker busy for this long before it composes, so a
    /// test can change the document while a build is in flight.
    void setComposeDelayForTest(int milliseconds) { m_composeDelayMs = milliseconds; }
    /// Clears the failed-build guard and tries once more.
    void retryComposition();

    std::function<void(const QString &)> onSelectionChanged;
    std::function<void(const QString &)> onSlotSelectionChanged;
    /// The sticker gesture that produced a commit, so the window can name the
    /// undo step without the view carrying interface text.
    enum class StickerAction { Move, Resize, Rotate };
    std::function<void(const QString &)> onStickerSelectionChanged;
    std::function<void(const Sticker &, StickerAction)> onStickerCommitted;
    std::function<void(const QString &)> onStickerDeleteRequested;
    /// The text gesture that produced a commit, so the window can name the undo
    /// step without the view carrying interface text.
    enum class TextAction { Move, Resize, Rotate };
    std::function<void(const QString &)> onTextSelectionChanged;
    std::function<void(const TextItem &, TextAction)> onTextCommitted;
    std::function<void(const QString &)> onTextDeleteRequested;
    /// A double click on a text asks the window to open its editor.
    std::function<void(const QString &)> onTextEditRequested;
    std::function<void(const Frame &)> onFrameCommitted;
    std::function<void(const QString &)> onDeleteRequested;
    std::function<void(const QString &)> onEmptySlotRequested;
    std::function<void()> onCropModeExited;
    /// Files from the file manager (second argument: the slot under the cursor,
    /// empty for the page background) and one frame dragged out of the library
    /// (frame id, slot id or empty when dropped outside every slot, and the drop
    /// point in page millimetres so a layout-free page can place it there).
    std::function<void(const QStringList &, const QString &)> onFilesDroppedAt;
    std::function<void(const QString &, const QString &, const QPointF &)> onFrameDroppedAt;
    std::function<void(double)> onViewZoomChanged;
    std::function<void(bool)> onCropModeChanged;
    std::function<void()> onCancelBusy;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    enum class DragKind {
        None,
        Move,
        Resize,
        Rotate,
        CropPan,
        MoveSticker,
        ResizeSticker,
        RotateSticker,
        MoveText,
        ResizeText,
        RotateText
    };

    struct DragState {
        DragKind kind = DragKind::None;
        int handle = -1;
        QPointF startPage;
        Frame startFrame;
        Sticker startSticker;
        TextItem startText;
        QSize pixels;
        bool changed = false;
    };

    /// One raster build, handed to a worker thread. Everything the worker needs
    /// is copied in: it never touches PageView or its members.
    struct ComposeJob {
        quint64 generation = 0;
        quint64 revision = 0;
        DocumentSnapshot document;
        PageImages images;
        QSizeF pageMm;
        QSize pixels;
        int delayMs = 0;
        bool forceFailure = false;
        std::shared_ptr<std::stop_source> stop;
    };

    struct ComposeResult {
        QImage image;
        QSizeF pageMm;
        quint64 generation = 0;
        quint64 revision = 0;
        bool cancelled = false;
    };

    const Frame *selectedFramePtr() const;
    const Sticker *selectedStickerPtr() const;
    const TextItem *selectedTextPtr() const;
    const Page *currentPage() const;
    QTransform pageToWidget() const;
    QPointF widgetToPage(const QPointF &point) const;
    QPolygonF frameQuadWidget(const Frame &frame) const;
    QPolygonF stickerQuadWidgetFor(const Sticker &sticker) const;
    QPolygonF textQuadWidgetFor(const TextItem &text) const;
    QPolygonF textHandleQuadWidgetFor(const TextItem &text) const;
    int hitFrame(const QPointF &widgetPoint) const;
    int hitSlot(const QPointF &widgetPoint) const;
    /// The corner handle under the pointer (0..3) or the rotate handle (4), for
    /// the selected sticker.
    int hitStickerHandle(const QPointF &widgetPoint) const;
    /// The same for the selected text block, but only in the outer ring: the
    /// interior is always a move, so a short caption can be dragged.
    int hitTextHandle(const QPointF &widgetPoint) const;
    int hitHandle(const QPointF &widgetPoint) const;
    SlotState slotStateFor(const Slot &slot) const;
    void paintSlotOverlays(QPainter &painter, const Page &page) const;
    /// The canvas-only placeholder for a sticker whose picture is lost: a red
    /// dashed box with a cross, drawn in widget space. The page raster, the PDF
    /// and the printer never carry it.
    void paintStickerPlaceholders(QPainter &painter, const Page &page) const;
    /// A dark outline that stays visible on a light page, even when the
    /// theme's text colour is light. The handles also carry a light underlay,
    /// so they stay visible on a dark background.
    QColor handleOutlineColor() const;
    /// Recomputes the base accessible description: the clipped-photo warning
    /// when the selected frame is partly outside its cell.
    void updateAccessibleDescription();
    void clearSelection();
    /// Recomputes the drag cue for the cell under the pointer and updates the
    /// accessible description with it, restoring the base text afterwards.
    void updateDropCue();
    void beginDrag(DragKind kind, int handle, const QPointF &pagePoint, const Frame &frame);
    void ensureComposed() const;
    void startCompose() const;
    void handleComposeFinished();
    void installCompose(const ComposeResult &result);
    void contentChanged(bool supersede);
    bool dragActive() const;
    bool interactionActive() const;
    bool interactionPulseActive() const;
    bool cropZoomPulseActive() const;
    bool nudgePulseActive() const;
    bool buildAllowed() const;
    void cancelInteraction(bool commitEdit);
    QSize targetRasterSize() const;
    QSize gestureRasterSize() const;
    bool rasterMatchesTarget() const;
    void settleRaster();
    void scheduleSettle();
    static QSize capRasterSize(QSize pixels, qint64 maxPixels);
    static ComposeResult composePage(const ComposeJob &job);
    void updateLiveFrame(const Frame &frame, bool markDragChanged = true);
    /// The live sticker under a drag. Like a frame, it is applied to the copy
    /// the canvas holds, so the composition follows the pointer and one commit
    /// lands when the button is released.
    void updateLiveSticker(const Sticker &sticker, bool markDragChanged = true);
    Sticker draggedSticker(const QPointF &pagePoint, bool fine) const;
    void beginStickerDrag(DragKind kind, int handle, const QPointF &pagePoint,
                          const Sticker &sticker);
    /// The live text under a drag, the same seam as a sticker.
    void updateLiveText(const TextItem &text, bool markDragChanged = true);
    TextItem draggedText(const QPointF &pagePoint, bool fine) const;
    void beginTextDrag(DragKind kind, int handle, const QPointF &pagePoint,
                       const TextItem &text);
    /// Lands an arrow-key nudge burst as one edit when the keys stop.
    void flushStickerNudge();
    void flushTextNudge();
    /// The end of a wheel burst publishes one edit, not one per notch, so one
    /// Ctrl+Z restores the photo's crop and zoom as the user saw it before.
    void flushWheelZoom();
    Frame draggedFrame(const QPointF &pagePoint, bool fine) const;
    void positionBusyOverlay();

    DocumentSnapshot m_document;
    PageImages m_images;
    QSet<QString> m_unavailable;
    QSet<QString> m_missingStickers;
    QString m_selectedId;
    QString m_selectedSlotId;
    QString m_selectedStickerId;
    QString m_selectedTextId;
    QString m_dropSlotId;
    QString m_dropCue;
    QString m_accessibleBase;
    QString m_emptyHint;
    bool m_cropMode = false;
    double m_viewZoom = 1.0;
    QPointF m_viewPan;
    bool m_panning = false;
    QPointF m_panStart;
    DragState m_drag;
    // Two different edit bursts: a crop zoom is a gesture and composes at the
    // fitted scale like a drag, an arrow nudge lets the photo trail and composes
    // once when the burst ends.
    QElapsedTimer m_cropZoomPulse;
    QElapsedTimer m_nudgePulse;
    // A wheel burst on a photo inside a slot: the live frame follows every
    // notch and one commit lands when the wheel stops.
    QTimer *m_zoomTimer = nullptr;
    QString m_zoomFrameId;
    Frame m_zoomStartFrame;
    Frame m_zoomCurrentFrame;
    /// The sticker nudge burst: the id being moved, the geometry it started
    /// from, and the timer that lands the whole burst as one edit.
    QTimer *m_stickerNudgeTimer = nullptr;
    QString m_nudgeStickerId;
    Sticker m_nudgeStartSticker;
    /// The text nudge burst, the same shape as the sticker one.
    QTimer *m_textNudgeTimer = nullptr;
    QString m_nudgeTextId;
    TextItem m_nudgeStartText;
    QColor m_canvasColor{0xd9, 0xd9, 0xde};
    QColor m_pageBorderColor{0xc7, 0xc7, 0xcc};
    QColor m_pageShadowColor{0, 0, 0, 38};
    // Page raster cache. A build runs on a worker thread from a copy of the
    // document and the prepared images and is installed by
    // handleComposeFinished(); the GUI thread only ever draws what is installed,
    // so no paint, pan, zoom or release waits for a composition.
    mutable QImage m_composed;
    mutable QSizeF m_composedPageMm;
    mutable bool m_contentDirty = true;
    mutable bool m_composeFailed = false;
    mutable int m_composeDelayMs = 0;
    mutable int m_blankRetries = 0;
    mutable quint64 m_contentRevision = 1;
    mutable quint64 m_composeGeneration = 0;
    mutable quint64 m_handledGeneration = 0;
    mutable quint64 m_composeRequests = 0;
    mutable quint64 m_compositions = 0;
    mutable bool m_failCompositions = false;
    mutable bool m_composeInFlight = false;
    mutable QFutureWatcher<ComposeResult> *m_composeWatcher = nullptr;
    mutable std::shared_ptr<std::stop_source> m_activeStop;
    QTimer *m_settleTimer = nullptr;
    QWidget *m_busyOverlay = nullptr;
    QLabel *m_busyLabel = nullptr;
    QPushButton *m_busyCancel = nullptr;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    DocumentSnapshot document() const { return m_controller.document(); }
    /// The cell the canvas has selected, or empty. A photo inside a layout
    /// selects its cell too; an empty cell can be selected on its own. The
    /// layout panel of the next slice reads this state.
    QString selectedSlot() const { return m_selectedSlotId; }

    /// Test hooks for the asynchronous page preview. The GUI thread starts a
    /// job and returns; the previous image stays installed until the matching
    /// result arrives.
    bool previewPending() const;
    bool waitForPreview(int timeoutMs = 5000);
    bool showingOriginal() const { return m_showOriginal; }
    bool originalPreviewPending() const { return m_originalJobActive; }
    bool waitForOriginalPreview(int timeoutMs = 5000);
    int originalRenderCountForTest() const { return m_originalRenderCount; }

    /// The confirmation the print path and the PDF export both show when a page
    /// carries printed cut guides. Public so a test can answer the box without a
    /// printer queue. Returns true when the job may proceed.
    bool confirmCutGuides();
    /// The same question for a document that is already narrowed to what will be
    /// exported (the current-page PDF, for instance), so the page numbers name
    /// the sheets in that file.
    bool confirmCutGuides(const DocumentSnapshot &document);
    /// The same question with the page numbers the caller will name, so a
    /// current-page export says "3" rather than renumbering it to 1.
    bool confirmCutGuides(const DocumentSnapshot &document, const QList<int> &pageNumbers);
    /// Applies one sheet-tool edit through the normal controller, one undo step.
    /// The same path the dialogs take, exposed so a test can drive it.
    Result<void> applySheetEdit(const ApplyLayoutEdit &edit);
    QUndoStack *undoStackForTest() { return m_controller.undoStack(); }
    /// Test hook: the sRGB previews the canvas is showing, keyed by frame id, so
    /// a test can build a sheet-tool dialog that paints real photographs.
    QHash<QString, QImage> previewImagesForTest() const { return m_previewImages; }
    /// Test hook: the frames the preview pass proved unreadable. The batch
    /// dialog's enable rule is built from this set.
    QSet<QString> unavailableFramesForTest() const { return m_unavailableFrames; }
    /// Test hook for the document-wide borderless rule the print path applies.
    bool borderlessConsistentForTest() const
    {
        return projectBorderlessConsistent(m_controller.document());
    }
    /// Test hook: the file-dialog-free entry point for adding a text, so a test
    /// can drive the same path the dialog lands on.
    bool addTextForTest(const TextItem &item);
    /// Test hook: the item `addText` would create, so a test can check the
    /// default placement and size without opening the modal dialog.
    TextItem newTextForTest() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    /// The text editor commits on focus out and on Ctrl+Enter, not per
    /// keystroke: a paragraph is one undo step.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// Which field of the page's layout style a control edits. A commit writes
    /// only this one, so an untouched control can never republish its clamped
    /// display over the stored value.
    enum class SlotStyleField { Margin, Gutter, Radius };

    /// Which background control a burst belongs to. A burst is landed before a
    /// different control reads the page, so a value the user just set is never
    /// overwritten by the next control's stale copy of the document.
    enum class BackgroundField { Kind, Solid, Stops, Angle, Image, Fit, Tile, Opacity };

    /// Why the page's background picture cannot be shown. `None` is a healthy
    /// picture or no picture at all; the other values carry a message for the
    /// panel and the notice bar.
    enum class BackgroundProblem { None, Missing, Unreadable, TooLarge };

private slots:
    void addPhotos();
    /// Fills the selected empty section with the pictures the picker returns. The
    /// panel is talking about that section, so the file lands there and not in
    /// whichever cell happens to be first.
    void addPhotosToSelectedSlot();
    /// The part of the fill that a test can drive without a file dialog: the
    /// paths go into the selected section, exactly as the button does.
    void addFilesToSelectedSlot(const QStringList &paths);
    void addDroppedFiles(const QStringList &paths);
    void paperChanged(int index);
    void orientationChanged(int index);
    void printerChanged(int index);
    void qualityChanged(int index);
    void colorChanged(int index);
    void manageCustomPapers();
    void refreshPrinters();
    void onDocumentChanged();
    void selectFrame(const QString &frameId, bool fromList);
    void applyFit(FitMode fit);
    void toggleCrop(bool enabled);
    void deleteSelected();
    void fitPage();
    void pageSelected(int index);
    void addPage();
    void duplicatePage();
    void removePage();
    void movePage(int delta);
    void reorderFrame(int delta);
    void savePdfCurrent();
    void savePdfAll();
    void saveProject();
    void saveProjectAs();
    void savePortableCopy();
    void openProject();
    void printNow();
    void showTrialSheet();
    void applyFilterSettings();
    void toggleAutoTone(bool enabled);
    void brightnessChanged(int value);
    /// The photograph's own rotation inside its frame. The slider, the quarter
    /// turns and the reset each write one FrameChange, so one action is one
    /// undo step. The frame angle stays a separate control.
    void imageRotationChanged(int degrees);
    void rotateImageBy(double degrees);
    void resetImageInsideFrame();
    /// Fits the selected slotted frame back onto its cell, one undo step.
    void replaceFrameInSlot();
    void showOriginalPressed(bool pressed);
    void applySizeFromControls();
    /// The page-level layout panel. A style edit names the one field the user
    /// touched, and that field alone is written: the other three are taken from
    /// the page, so a value the current paper clamps on screen is never written
    /// back over the stored one.
    ///
    /// `queueSlotStyle` runs on every value change and restarts a short timer, so
    /// the page follows the panel while the spin is still being used and the
    /// whole burst becomes one edit. `flushSlotStyle` lands it now, and is what
    /// editingFinished, the timer and a paper change call.
    void queueSlotStyle(SlotStyleField field);
    void flushSlotStyle();
    void cancelPendingStyle();
    void applySlotStyleField(SlotStyleField field);
    void addSlotToPage();
    void removeSlotFromPage();
    void dissolvePageLayout();
    void showDiagnostics();
    void saveSettings();
    void restoreSettings();
    /// Restores a loaded project. It is a slot so a test can deliver a project
    /// without driving the file dialog; the recovery and open paths use it
    /// directly.
    void applyLoadedProject(const LoadedProject &loaded, bool recovery = false);
    /// The collage template picker for the current page. Browsing never touches
    /// the document; the chosen template comes back to applyTemplate().
    void openTemplateBrowser();
    /// The two sheet tools. Browsing is dialog-local; accepting one edit lands
    /// through applySheetEdit and one undo step restores the page list.
    void openBatchCopies();
    void openAlignPhotos();
    /// The per-page cut guide state. Changing it is an undoable edit, so the
    /// state a project was printed with can always be recovered.
    void guidesChanged(int index);
    /// The per-page borderless override. Auto follows the printer; Off and On
    /// pin the decision so a batch sheet prints at its exact copy size.
    void borderlessChanged(int index);
    /// The page background. Every control writes the whole `PageBackground` as
    /// one page edit, so one action is one undo step. The opacity slider and the
    /// tile spin are bursts: the page follows live and the burst lands once.
    void backgroundKindChanged(int index);
    void chooseBackgroundColor();
    void chooseGradientStart();
    void chooseGradientEnd();
    /// Shared body of the two gradient swatches: `which` 0 is the first stop,
    /// 1 the last. Extra stops from a future build are preserved.
    void chooseGradientStop(int which);
    void gradientAngleChanged(int index);
    void chooseBackgroundImage();
    void backgroundFitChanged(int index);
    void backgroundTileChanged(double value);
    void backgroundOpacityChanged(int value);
    void resetBackground();
    void queueBackground(const PageBackground &background, BackgroundField field);
    void flushBackground();
    void cancelPendingBackground();
    /// The page's background with any pending burst landed first when the new
    /// edit belongs to a different control. This is the background counterpart
    /// of queueSlotStyle's field guard.
    PageBackground currentBackgroundFor(BackgroundField field);
    /// The sticker layer. A file dialog or the built-in set adds one at the page
    /// centre, above every existing overlay, and selects it. Each property
    /// control writes one page edit, so one action is one undo step.
    void addSticker();
    /// The file-dialog-free entry point: the built-in set, the relink picker and
    /// the tests all land here.
    void addStickerFromPath(const QString &path);
    /// Selects a sticker, or clears the overlay selection with an empty id. A
    /// slot so a test can drive the same path the canvas uses.
    void selectSticker(const QString &stickerId);
    /// Adds one of the shipped stickers. The resource is copied into the app's
    /// own data folder first, so the project stores a real file path that a
    /// portable copy can carry like a photograph.
    void addBuiltInSticker(const QString &alias);
    /// Picks a replacement file for a sticker whose picture is lost.
    void chooseStickerImage();
    /// The file-dialog-free relink: the picker and the tests land here.
    void relinkStickerFromPath(const QString &path);
    /// Lands a pending arrow-key nudge before another edit reads the page, so a
    /// panel action inside the 200 ms burst cannot publish a stale position.
    void flushPendingStickerEdit();
    /// The same for a text block's nudge burst.
    void flushPendingTextEdit();
    /// Lands both pending bursts. A shared-stack action (add, reorder, delete)
    /// touches both kinds, so it must not leave either burst to publish a stale
    /// page afterwards.
    void flushPendingOverlayEdits();
    void stickerSizeChanged();
    void stickerAngleChanged();
    void stickerOpacityChanged(int value);
    /// Lands the opacity burst as one edit. The slider previews live, so a drag
    /// is one undo step instead of one per pixel.
    void flushStickerOpacity();
    void cancelPendingStickerOpacity();
    /// Moves the selected sticker one real step in the paint order.
    void reorderStickerLayer(int delta);
    void deleteSelectedSticker();
    /// The text layer. `Beszúrás -> Szöveg…` and the Elem tab's add button both
    /// land here; the dialog is the only place a text is created or edited.
    void addText();
    /// The item `addText` starts the dialog with: the lower margin below the
    /// content (or the lower third), 14 pt, the last family and colour.
    TextItem newTextItem() const;
    /// Opens the same dialog for the selected text, so editing reuses it.
    void editSelectedText();
    /// Selects a text, or clears the overlay selection with an empty id.
    void selectText(const QString &textId);
    /// The Elem tab's text controls. Each one writes the whole page as one edit,
    /// so one action is one undo step.
    void textContentChanged();
    void textFontChanged(int index);
    void textSizeChanged();
    void textStyleChanged();
    void chooseTextColor();
    /// Applies one of the three alignments. The buttons are one choice, so the
    /// clicked value is passed in rather than read back from the toggle states.
    void applyTextAlign(TextAlign align);
    void textSpacingChanged();
    void textAngleChanged();
    void textOpacityChanged(int value);
    void flushTextOpacity();
    void cancelPendingTextOpacity();
    /// Stops the typing debounce and rewrites the editor from the document, so
    /// an undo or a page switch cannot leave a stale paragraph in the field.
    void cancelPendingTextEdit();
    void deleteSelectedText();
    /// Moves the selected overlay (sticker or text) one real step in the shared
    /// stack, so raising a sticker above a text swaps them.
    void reorderOverlayLayer(int delta);
    /// Selects the previous (-1) or next (+1) overlay in the shared paint order.
    void cycleOverlay(int delta);
    /// Applies one catalogue template to the current page as one page edit, so
    /// one undo restores the layout. The slot engine creates the cells (and the
    /// layout label) and assigns the photographs in display order; the page's
    /// own layout is topped up, any other page is laid out from scratch. Fewer
    /// photographs than cells is legal and the surplus stays free. Returns false
    /// and says why when the engine refuses.
    bool applyTemplate(const QString &templateId);

private:
    void buildUi();
    void buildMenus();
    void buildLanguageMenu();
    void rebuildUi();
    void setLanguage(const QString &code);
    void showAbout();
    void addPhoto(const QString &path);
    /// Files dropped on the canvas, or picked by "Fotók hozzáadása": a slot id
    /// sends them there, an empty one is the page background (first empty slot
    /// on a laid-out page). One edit, one undo.
    void addFilesAt(const QStringList &paths, const QString &slotId);
    /// One photo dragged out of the library: move or swap into a slot, detach to
    /// a free frame outside the layout, or place it on a layout-free page at the
    /// drop point.
    void dropFrameAt(const QString &frameId, const QString &slotId, const QPointF &pagePoint);
    /// Removes the photograph in a cell and keeps the cell empty.
    void clearSlot(const QString &slotId);
    /// Selects a cell; a filled cell selects its photograph instead.
    void selectSlot(const QString &slotId);
    std::optional<Frame> photoPayload(const QString &path, const Page &page);
    void refreshPhotoList();
    void rebuildPreviewImages();
    void startPreviewJob();
    void handlePreviewFinished();
    void startOriginalPreview(const Frame &frame);
    void handleOriginalPreviewFinished();
    void cancelPreviewWork();
    void updateBusyOverlay();
    QString sourceKeyFor(const Frame &frame) const;
    /// The layers the canvas should show: the preview frames plus the background
    /// picture, but only when the loaded picture belongs to the current page.
    /// A stale background from the previous page is never stretched over this
    /// one while the new preview job runs.
    PageImages displayedPageImages() const;
    void updateResolutionHint();
    void openEffectBrowser();
    void applyEffectChoice(const QString &key, int version, int strength);
    void syncFrameControls();
    void syncLayoutPanel();
    /// Writes the selected section's facts into the Hely tab: its cell size, its
    /// position on the page, whether it holds a photograph and whether that
    /// photograph is clipped. The panel shows the same section the canvas has
    /// selected, so these are the numbers it is talking about.
    void syncSlotFacts(const Page &page, const QString &slotId);
    /// The contextual inspector: Fotó, Hely and Lap. Only the selected photo's
    /// tab, only the selected section's tab, otherwise Lap. `frameId` is the
    /// selected photograph, `slotId` the selected section (a photograph also
    /// selects its own cell, so Hely can describe it), `emptySection` says
    /// whether that section has no photograph yet. The selection context decides;
    /// a manual tab choice survives until that context changes, so a slider never
    /// flips the panel under the pointer. Tabs without content are disabled, Lap
    /// never is.
    void updateInspectorTabs(const QString &frameId, const QString &slotId,
                             bool emptySection, const QString &overlayId,
                             bool overlayIsText);
    /// Switches the inspector tab programmatically. A user switch goes through
    /// QTabWidget itself, which is what records the manual choice.
    void setInspectorTab(int index);
    /// Writes the photograph's own rotation into the slider and its readout,
    /// skipping the slider while the user is dragging it.
    void syncImageRotationControls(const Frame &frame);
    /// The page-level sheet controls: the align button and the cut guide combo,
    /// plus the batch entry's enabled state.
    void syncPageTools();
    /// Writes the current page's background into the Lap panel: the kind, the
    /// colours, the angle, the picture, the fit, the tile size and the opacity.
    /// Only the controls of the selected kind are visible, so the panel never
    /// shows a fit combo for a solid page.
    void syncBackgroundPanel();
    /// Writes the selected sticker into the Elem tab: the preview, the size, the
    /// rotation, the opacity and the two order buttons. With nothing selected the
    /// tab shows its empty state, which is also where a new sticker starts.
    void syncStickerPanel();
    std::optional<Sticker> selectedSticker() const;
    /// Writes the selected text into the Elem tab: the editor, the font, the
    /// size, the style toggles, the colour, the alignment, the spacing, the
    /// opacity and the two order buttons. With nothing selected the tab shows
    /// its empty state.
    void syncTextPanel();
    std::optional<TextItem> selectedText() const;
    /// Applies a text list to the current page as one undo step and selects
    /// `selectId` when it is given.
    bool publishTexts(const QList<TextItem> &texts, const QString &actionText,
                      const QString &selectId = QString());
    /// Places a text whose geometry is already decided: the dialog, the tests
    /// and the default placement all land here.
    bool addTextWithItem(const TextItem &item);
    /// Replaces the selected text with `item` as one undo step. An emptied text
    /// deletes the item in the same step, so the page never holds an invisible
    /// layer.
    void applyTextEdit(const TextItem &item);
    /// Lands the editor's content as one edit. Called on focus out, on
    /// Ctrl+Enter and when the selection changes.
    void commitTextEditor();
    /// True when the stored family is not installed on this machine, so the
    /// panel can say that Qt substitutes it.
    bool textFontMissing() const;
    /// True when the block does not fit the sheet, so the panel can say that
    /// part of it falls off the page.
    bool textOverflows() const;
    /// Applies a sticker list to the current page as one undo step and selects
    /// `selectId` when it is given.
    bool publishStickers(const QList<Sticker> &stickers, const QString &actionText,
                         const QString &selectId = QString());
    /// Places a sticker whose pixels are already identified: the stored path
    /// (a file path or a shipped alias), its identity and its pixel size. The
    /// file dialog, the built-in set and the relink picker all land here.
    bool addStickerWithSource(const QString &storedPath, const QByteArray &sha256,
                              const QSize &pixels);
    /// Why a sticker's picture cannot be shown. `None` is a healthy one,
    /// `Changed` means the file under a pinned alias no longer matches the
    /// content the project recorded.
    enum class StickerProblem { None, Missing, Unreadable, TooLarge, Changed };
    StickerProblem stickerProblem(const Sticker &sticker) const;
    QString stickerProblemText(StickerProblem problem) const;
    /// The first problem on the current page, so the notice bar speaks once.
    StickerProblem pageStickerProblem() const;
    /// A small preview icon, decoded at its final size so a large source is
    /// never decoded in full on the GUI thread.
    QPixmap stickerThumbnail(const Sticker &sticker, int size) const;
    /// The precise reason the picture cannot be shown, so the panel and the
    /// notice bar can say "not found" or "too large or unreadable" instead of
    /// leaving an empty strip.
    BackgroundProblem backgroundProblem() const;
    QString backgroundProblemText(BackgroundProblem problem) const;
    /// The page's layout name for the panel: the English catalogue name when the
    /// layout came from the catalogue, a neutral label for a hand-made one.
    QString layoutDisplayName(const Page &page) const;
    void syncListSelection();
    void showProblem(const QString &message);
    void exportPdf(bool allPages);
    void submitPrint(const DocumentSnapshot &document, const PrintTicket &ticket);
    void updatePrintButton();
    void updateNoticeBar();
    void applyTheme();
    void rebuildPaperCombo(const QString &selectedName);
    void ensurePaperInCombo(const PaperSpec &paper);
    void syncPrintControls();
    bool wantBorderless(const Page &page) const;
    void promptMissingSources();
    void checkpointNow();
    void updateTitle();
    Frame makeInitialFrame(const SourceImage &source, const Page &page) const;
    /// Small sRGB thumbnails in frame order, for the template previews. A photo
    /// that is not cached yet is left null rather than loaded on the GUI thread.
    /// The list is cached by frame identity, because a document change arrives
    /// for every nudge and every selection and the pixels did not change.
    QList<QImage> templatePhotoThumbnails(const Page &page);
    bool pageOverflows() const;
    bool projectPrintGeometryConsistent(const DocumentSnapshot &document) const;
    /// The print path sends one ticket for the whole job, so pages that disagree
    /// about borderless cannot be printed honestly.
    bool projectBorderlessConsistent(const DocumentSnapshot &document) const;
    std::optional<Frame> selectedFrame() const;
    PaperSpec currentPaper() const;
    const PrinterCapabilities *printerByQueue(const QString &queue) const;

    /// One preview build. The worker receives a copy of everything it needs and
    /// never touches the window; a newer generation cancels it between stages.
    struct PreviewJob {
        quint64 generation = 0;
        DocumentSnapshot document;
        int pageIndex = 0;
        std::shared_ptr<std::stop_source> stop;
    };

    struct PreviewResult {
        quint64 generation = 0;
        int pageIndex = -1;
        QHash<QString, QImage> images;
        QImage background;
        QSize backgroundPixels;
        QString backgroundPath;
        QHash<QString, QImage> stickers;
        QSet<QString> missingStickers;
    };

    /// One untouched layer, rendered only when the user first asks for it and
    /// cached by source identity, so two frames of the same photo share it.
    struct OriginalJob {
        quint64 generation = 0;
        QString sourceKey;
        QString sourcePath;
        QByteArray sha256;
        ColorRecipe recipe;
        std::shared_ptr<std::stop_source> stop;
    };

    struct OriginalResult {
        quint64 generation = 0;
        QString sourceKey;
        QImage image;
    };

    CanvasController m_controller;
    ImageStore m_store;
    ImageStore m_printStore;
    PageRenderer m_renderer;

    PageView *m_pageView = nullptr;
    QListWidget *m_photoList = nullptr;
    QComboBox *m_printerCombo = nullptr;
    QComboBox *m_paperCombo = nullptr;
    QComboBox *m_orientationCombo = nullptr;
    QComboBox *m_zoomCombo = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QComboBox *m_colorCombo = nullptr;
    QPushButton *m_customPaperButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_fitButton = nullptr;
    QPushButton *m_fillButton = nullptr;
    QPushButton *m_cropButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_forwardButton = nullptr;
    QAbstractButton *m_autoCheck = nullptr;
    QPushButton *m_effectButton = nullptr;
    QLabel *m_legacyHint = nullptr;
    QSlider *m_filterStrength = nullptr;
    QLabel *m_strengthLabel = nullptr;
    QLabel *m_strengthHint = nullptr;
    QCheckBox *m_upscaleCheck = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QPushButton *m_originalButton = nullptr;
    QSpinBox *m_copiesSpin = nullptr;
    QPushButton *m_printButton = nullptr;
    QPushButton *m_trialButton = nullptr;
    QPushButton *m_batchButton = nullptr;
    QPushButton *m_alignButton = nullptr;
    QComboBox *m_guidesCombo = nullptr;
    QLabel *m_guidesHint = nullptr;
    QComboBox *m_borderlessCombo = nullptr;
    QLabel *m_borderlessHint = nullptr;
    QPushButton *m_backwardButton = nullptr;
    QDoubleSpinBox *m_widthSpin = nullptr;
    QDoubleSpinBox *m_heightSpin = nullptr;
    QDoubleSpinBox *m_angleSpin = nullptr;
    /// The photograph's own rotation inside its frame, kept compact inside the
    /// Fotó tab. The frame angle is the separate spin above; this one turns the
    /// picture, not the window.
    QSlider *m_imageRotationSlider = nullptr;
    QLabel *m_imageRotationValue = nullptr;
    QPushButton *m_imageRotateLeftButton = nullptr;
    QPushButton *m_imageRotateRightButton = nullptr;
    QPushButton *m_imageResetButton = nullptr;
    /// Fits a slotted frame back onto its cell. Hidden for a free frame.
    QPushButton *m_replaceInSlotButton = nullptr;
    /// Says that part of the photograph is outside its cell and is clipped.
    QLabel *m_clippedHint = nullptr;
    /// The page's own inspector: visible whenever the page has slots, even with
    /// no photograph selected. It is the content of the Lap tab, so it cannot
    /// coexist with the selected-photograph sections any more.
    QWidget *m_layoutPanel = nullptr;
    /// The contextual inspector. Three stable indices: Fotó (0), Hely (1) and
    /// Lap (2). The selection decides which one opens; a tab with no content is
    /// disabled instead of removed, so the indices and the tab order stay put.
    /// The tab strip is outside the pages; each page owns its scroll area.
    QTabWidget *m_inspectorTabs = nullptr;
    /// The Lap tab's no-layout state: one line saying the page has no layout, so
    /// a free page is not a stack of controls that would only clamp to zero.
    QWidget *m_pageSummaryPanel = nullptr;
    /// The Hely tab's section facts: size in millimetres, position, occupancy and
    /// clipping state of the selected cell.
    QLabel *m_slotSizeValue = nullptr;
    QLabel *m_slotPositionValue = nullptr;
    QLabel *m_slotStateValue = nullptr;
    QLabel *m_slotClipValue = nullptr;
    QPushButton *m_slotFillButton = nullptr;
    QPushButton *m_slotRemoveButton = nullptr;
    /// What the inspector was showing last: the selected photograph, the
    /// selected empty section, or nothing. A change re-opens the matching tab;
    /// a manual choice holds until then.
    QString m_inspectorContext;
    bool m_inspectorTabChosen = false;
    bool m_settingInspectorTab = false;
    QLabel *m_layoutNameLabel = nullptr;
    QLabel *m_layoutCountLabel = nullptr;
    QDoubleSpinBox *m_marginSpin = nullptr;
    QCheckBox *m_marginAutoCheck = nullptr;
    QDoubleSpinBox *m_gutterSpin = nullptr;
    QCheckBox *m_gutterAutoCheck = nullptr;
    QLabel *m_gutterHint = nullptr;
    QDoubleSpinBox *m_radiusSpin = nullptr;
    QPushButton *m_addSlotButton = nullptr;
    QPushButton *m_dissolveLayoutButton = nullptr;
    /// The Lap tab's background group. The panel is always visible: a free page
    /// has a background too, and it is the one page property that is not about
    /// the layout.
    QWidget *m_backgroundPanel = nullptr;
    QComboBox *m_backgroundKindCombo = nullptr;
    QWidget *m_backgroundSolidRow = nullptr;
    QPushButton *m_backgroundColorButton = nullptr;
    QWidget *m_backgroundGradientRow = nullptr;
    QPushButton *m_gradientStartButton = nullptr;
    QPushButton *m_gradientEndButton = nullptr;
    QComboBox *m_gradientAngleCombo = nullptr;
    QLabel *m_gradientHint = nullptr;
    QWidget *m_backgroundImageRow = nullptr;
    QPushButton *m_backgroundImageButton = nullptr;
    QLabel *m_backgroundFileLabel = nullptr;
    QComboBox *m_backgroundFitCombo = nullptr;
    QLabel *m_backgroundTileLabel = nullptr;
    QDoubleSpinBox *m_backgroundTileSpin = nullptr;
    QSlider *m_backgroundOpacitySlider = nullptr;
    QLabel *m_backgroundOpacityLabel = nullptr;
    QLabel *m_backgroundOpacityValue = nullptr;
    QPushButton *m_backgroundResetButton = nullptr;
    QLabel *m_backgroundHint = nullptr;
    QLabel *m_backgroundMissingHint = nullptr;
    QPushButton *m_backgroundRelinkButton = nullptr;
    /// The notice bar's own line for a background picture that cannot be shown.
    QLabel *m_backgroundNotice = nullptr;
    /// The Elem tab: the sticker properties of the selected layer, and the
    /// palette that adds one. The palette stays visible with a sticker selected,
    /// so a second one never needs a deselect first.
    QWidget *m_stickerAddPanel = nullptr;
    QWidget *m_stickerPanel = nullptr;
    QWidget *m_stickerEmptyPanel = nullptr;
    QLabel *m_stickerEmptyHint = nullptr;
    QLabel *m_stickerPreview = nullptr;
    QDoubleSpinBox *m_stickerSizeSpin = nullptr;
    QDoubleSpinBox *m_stickerAngleSpin = nullptr;
    QSlider *m_stickerOpacitySlider = nullptr;
    QLabel *m_stickerOpacityValue = nullptr;
    QPushButton *m_stickerRaiseButton = nullptr;
    QPushButton *m_stickerLowerButton = nullptr;
    QPushButton *m_stickerDeleteButton = nullptr;
    QPushButton *m_addStickerButton = nullptr;
    QWidget *m_stickerSetGrid = nullptr;
    QLabel *m_stickerPositionLabel = nullptr;
    QPushButton *m_previousStickerButton = nullptr;
    QPushButton *m_nextStickerButton = nullptr;
    QLabel *m_stickerMissingHint = nullptr;
    QPushButton *m_stickerRelinkButton = nullptr;
    /// The notice bar's own line for a sticker that cannot be shown.
    QLabel *m_stickerNotice = nullptr;
    /// The way in from that line: selects the first missing sticker, so the
    /// relink picker is one click away instead of three.
    QPushButton *m_stickerNoticeButton = nullptr;
    /// The Elem tab's text half: the add button, the selected text's controls
    /// and the two hints. The panel switches between the sticker and the text
    /// properties, so only the selected kind's controls are visible.
    QWidget *m_textAddPanel = nullptr;
    QWidget *m_textPanel = nullptr;
    QPushButton *m_editTextButton = nullptr;
    QPlainTextEdit *m_textEditor = nullptr;
    QFontComboBox *m_textFontCombo = nullptr;
    QDoubleSpinBox *m_textSizeSpin = nullptr;
    QDoubleSpinBox *m_textAngleSpin = nullptr;
    QToolButton *m_textBoldButton = nullptr;
    QToolButton *m_textItalicButton = nullptr;
    QToolButton *m_textUnderlineButton = nullptr;
    QToolButton *m_textAlignLeftButton = nullptr;
    QToolButton *m_textAlignCenterButton = nullptr;
    QToolButton *m_textAlignRightButton = nullptr;
    QPushButton *m_textColorButton = nullptr;
    QDoubleSpinBox *m_textSpacingSpin = nullptr;
    QSlider *m_textOpacitySlider = nullptr;
    QLabel *m_textOpacityValue = nullptr;
    QPushButton *m_textRaiseButton = nullptr;
    QPushButton *m_textLowerButton = nullptr;
    QPushButton *m_textDeleteButton = nullptr;
    QPushButton *m_addTextButton = nullptr;
    QLabel *m_textFontHint = nullptr;
    QLabel *m_textOverflowHint = nullptr;
    /// The text opacity burst: the canvas follows live and one edit lands when
    /// the burst stops.
    QTimer *m_textOpacityTimer = nullptr;
    /// The typing debounce: the canvas follows every keystroke and one edit
    /// lands when the typing stops, so a paragraph is one undo step.
    QTimer *m_textEditTimer = nullptr;
    std::optional<TextItem> m_pendingTextOpacity;
    QString m_selectedText;
    /// The last family, size and colour a text was created with, so a second
    /// caption does not restart from the defaults.
    QString m_lastTextFamily;
    double m_lastTextPt = 0.0;
    QRgb m_lastTextColor = 0xff000000u;
    /// True while the editor holds a change the document has not seen yet.
    bool m_textEditorDirty = false;
    /// The canvas layers of the current page's stickers, loaded by the preview
    /// job so the canvas and the print cannot disagree about what is on the page.
    QHash<QString, QImage> m_previewStickers;
    QSet<QString> m_missingStickers;
    /// The file the panel preview was decoded from, so a document change does
    /// not re-read the picture on every nudge.
    QString m_stickerPreviewPath;
    QPixmap m_stickerPreviewPixmap;
    /// The opacity burst: the canvas follows live and one edit lands when the
    /// drag stops. The nudge burst belongs to PageView, which owns the geometry.
    QTimer *m_stickerOpacityTimer = nullptr;
    std::optional<Sticker> m_pendingStickerOpacity;
    QString m_lastStickerDir;
    QString m_selectedSticker;
    /// The live background burst: every opacity or tile change restarts the
    /// timer, and the whole burst lands as one undo step.
    QTimer *m_backgroundTimer = nullptr;
    std::optional<PageBackground> m_pendingBackground;
    std::optional<BackgroundField> m_pendingBackgroundField;
    /// The live style burst: every value change restarts the timer, and the page
    /// is updated once the user stops, so a spin that is held down is one undo.
    QTimer *m_styleTimer = nullptr;
    std::optional<SlotStyleField> m_pendingStyleField;
    PagesPanel *m_pagesPanel = nullptr;
    QLabel *m_pageInfo = nullptr;
    QWidget *m_noticeBar = nullptr;
    QWidget *m_inspectorPanel = nullptr;
    QLabel *m_printerStatus = nullptr;
    QLabel *m_paperHint = nullptr;
    QLabel *m_overflowLabel = nullptr;
    QPushButton *m_fitPageButton = nullptr;
    QPushButton *m_recheckButton = nullptr;
    QLabel *m_resolutionHint = nullptr;
    QLabel *m_autoHint = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    /// The visible way into the full text editor, next to the panel button.
    QAction *m_editTextAction = nullptr;
    QTimer *m_resizeTimer = nullptr;

    bool m_syncing = false;
    QString m_selectedId;
    QString m_selectedSlotId;
    QHash<QString, QImage> m_previewImages;
    QImage m_previewBackground;
    QSize m_previewBackgroundPixels;
    QString m_previewBackgroundPath;
    /// Frames whose source could not be decoded on the last preview pass, so a
    /// slot can say "not found" instead of pretending to load forever. A frame
    /// that is simply not decoded yet is not in here.
    QSet<QString> m_unavailableFrames;
    QHash<QString, QImage> m_originalPreviews;
    QPointer<EffectBrowserDialog> m_effectDialog;
    QPointer<TemplateBrowserDialog> m_templateDialog;
    QStringList m_templatePhotoKeys;
    QList<QImage> m_templatePhotos;
    QFutureWatcher<PreviewResult> *m_previewWatcher = nullptr;
    std::shared_ptr<std::stop_source> m_previewStop;
    quint64 m_previewGeneration = 0;
    bool m_previewDirty = false;
    bool m_previewJobActive = false;
    QFutureWatcher<OriginalResult> *m_originalWatcher = nullptr;
    std::shared_ptr<std::stop_source> m_originalStop;
    quint64 m_originalGeneration = 0;
    bool m_originalJobActive = false;
    QString m_originalJobSourceKey;
    int m_originalRenderCount = 0;
    QList<PaperSpec> m_papers;
    CustomPaperStore m_customPaperStore;
    PrinterCatalog m_catalog;
    QList<PrinterCapabilities> m_printers;
    QString m_queue;
    bool m_restoring = false;
    bool m_restoringProject = false;
    bool m_dirty = false;
    bool m_showOriginal = false;
    bool m_recoveryEnabled = true;
    QString m_projectPath;
    QList<SourceIssue> m_sourceIssues;
    RecoveryStore m_recovery;
    QTimer *m_checkpointTimer = nullptr;
    RealCupsBackend m_cups;
    std::unique_ptr<JobController> m_jobController;
    bool m_printInProgress = false;
    QFutureWatcher<Result<JobResult>> *m_printWatcher = nullptr;
    PaperPresetStore m_presets;
    QString m_inkIdentity = QStringLiteral("unknown");
    QString m_paperName;
    bool m_landscape = false;
};

} // namespace pp
