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
class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

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
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    enum class DragKind { None, Move, Resize, Rotate, CropPan };

    struct DragState {
        DragKind kind = DragKind::None;
        int handle = -1;
        QPointF startPage;
        Frame startFrame;
        QSize pixels;
        bool changed = false;
    };

    /// One raster build, handed to a worker thread. Everything the worker needs
    /// is copied in: it never touches PageView or its members.
    struct ComposeJob {
        quint64 generation = 0;
        quint64 revision = 0;
        DocumentSnapshot document;
        QHash<QString, QImage> images;
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
    const Page *currentPage() const;
    QTransform pageToWidget() const;
    QPointF widgetToPage(const QPointF &point) const;
    QPolygonF frameQuadWidget(const Frame &frame) const;
    int hitFrame(const QPointF &widgetPoint) const;
    int hitSlot(const QPointF &widgetPoint) const;
    int hitHandle(const QPointF &widgetPoint) const;
    SlotState slotStateFor(const Slot &slot) const;
    void paintSlotOverlays(QPainter &painter, const Page &page) const;
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
    /// The end of a wheel burst publishes one edit, not one per notch, so one
    /// Ctrl+Z restores the photo's crop and zoom as the user saw it before.
    void flushWheelZoom();
    Frame draggedFrame(const QPointF &pagePoint, bool fine) const;
    void positionBusyOverlay();

    DocumentSnapshot m_document;
    QHash<QString, QImage> m_images;
    QSet<QString> m_unavailable;
    QString m_selectedId;
    QString m_selectedSlotId;
    QString m_dropSlotId;
    QString m_dropCue;
    QString m_accessibleBeforeDrag;
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
    /// Applies one sheet-tool edit through the normal controller, one undo step.
    /// The same path the dialogs take, exposed so a test can drive it.
    Result<void> applySheetEdit(const ApplyLayoutEdit &edit);
    QUndoStack *undoStackForTest() { return m_controller.undoStack(); }
    /// Test hook: the sRGB previews the canvas is showing, keyed by frame id, so
    /// a test can build a sheet-tool dialog that paints real photographs.
    QHash<QString, QImage> previewImagesForTest() const { return m_previewImages; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    /// Which field of the page's layout style a control edits. A commit writes
    /// only this one, so an untouched control can never republish its clamped
    /// display over the stored value.
    enum class SlotStyleField { Margin, Gutter, Border, Radius };

private slots:
    void addPhotos();
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
    /// The colour dialog's answer, in one function, so the panel has one writer
    /// for the border colour and a test can deliver a colour without a modal
    /// dialog.
    void applySlotBorderColour(const QColor &colour);
    void chooseSlotBorderColour();
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
    QHash<QString, QImage> displayedPreviewImages() const;
    void updateResolutionHint();
    void openEffectBrowser();
    void applyEffectChoice(const QString &key, int version, int strength);
    void syncFrameControls();
    void syncLayoutPanel();
    /// The page-level sheet controls: the align button and the cut guide combo,
    /// plus the batch entry's enabled state.
    void syncPageTools();
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
    QPushButton *m_backwardButton = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QDoubleSpinBox *m_widthSpin = nullptr;
    QDoubleSpinBox *m_heightSpin = nullptr;
    QDoubleSpinBox *m_angleSpin = nullptr;
    /// The page's own inspector: visible whenever the page has slots, even with
    /// no photograph selected. The selected-photo sections live in
    /// m_photoPanel, so the two can coexist without hiding each other.
    QWidget *m_layoutPanel = nullptr;
    QWidget *m_photoPanel = nullptr;
    QLabel *m_layoutNameLabel = nullptr;
    QLabel *m_layoutCountLabel = nullptr;
    QDoubleSpinBox *m_marginSpin = nullptr;
    QCheckBox *m_marginAutoCheck = nullptr;
    QDoubleSpinBox *m_gutterSpin = nullptr;
    QCheckBox *m_gutterAutoCheck = nullptr;
    QDoubleSpinBox *m_borderSpin = nullptr;
    QPushButton *m_borderColourButton = nullptr;
    QDoubleSpinBox *m_radiusSpin = nullptr;
    QPushButton *m_addSlotButton = nullptr;
    QPushButton *m_removeSlotButton = nullptr;
    QPushButton *m_dissolveLayoutButton = nullptr;
    QPushButton *m_layoutTemplatesButton = nullptr;
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
    QTimer *m_resizeTimer = nullptr;

    bool m_syncing = false;
    QString m_selectedId;
    QString m_selectedSlotId;
    QHash<QString, QImage> m_previewImages;
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
