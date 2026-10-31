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
#include <QMainWindow>
#include <QPointer>

#include <QFutureWatcher>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace pp {

class EffectBrowserDialog;

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
    void setCropMode(bool enabled);
    bool cropMode() const { return m_cropMode; }
    void setEmptyHint(const QString &text);
    void setThemeColors(const QColor &canvas, const QColor &border, const QColor &shadow);

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
    std::function<void(const Frame &)> onFrameCommitted;
    std::function<void(const QString &)> onDeleteRequested;
    std::function<void()> onCropModeExited;
    std::function<void(const QStringList &)> onFilesDropped;
    std::function<void(double)> onViewZoomChanged;
    std::function<void(bool)> onCropModeChanged;

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
    QTransform pageToWidget() const;
    QPointF widgetToPage(const QPointF &point) const;
    QPolygonF frameQuadWidget(const Frame &frame) const;
    int hitFrame(const QPointF &widgetPoint) const;
    int hitHandle(const QPointF &widgetPoint) const;
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
    void updateLiveFrame(const Frame &frame);
    Frame draggedFrame(const QPointF &pagePoint, bool fine) const;

    DocumentSnapshot m_document;
    QHash<QString, QImage> m_images;
    QString m_selectedId;
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
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    DocumentSnapshot document() const { return m_controller.document(); }

    /// Test hooks for the asynchronous page preview. The GUI thread starts a
    /// job and returns; the previous image stays installed until the matching
    /// result arrives.
    bool previewPending() const;
    bool waitForPreview(int timeoutMs = 5000);
    bool showingOriginal() const { return m_showOriginal; }

protected:
    void closeEvent(QCloseEvent *event) override;

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
    void showDiagnostics();
    void saveSettings();
    void restoreSettings();
    /// Restores a loaded project. It is a slot so a test can deliver a project
    /// without driving the file dialog; the recovery and open paths use it
    /// directly.
    void applyLoadedProject(const LoadedProject &loaded, bool recovery = false);

private:
    void buildUi();
    void buildMenus();
    void buildLanguageMenu();
    void rebuildUi();
    void setLanguage(const QString &code);
    void showAbout();
    void addPhoto(const QString &path);
    void refreshPhotoList();
    void rebuildPreviewImages();
    void startPreviewJob();
    void handlePreviewFinished();
    void ensureOriginalPreview(const QString &frameId);
    QHash<QString, QImage> displayedPreviewImages() const;
    void updateResolutionHint();
    void openEffectBrowser();
    void applyEffectChoice(const QString &key, int version, int strength);
    void setSelectedEffectStrength(int strength);
    void syncFrameControls();
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
    bool wantBorderless(const PaperSpec &paper) const;
    void promptMissingSources();
    void checkpointNow();
    void updateTitle();
    Frame makeInitialFrame(const SourceImage &source, const Page &page) const;
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
        QHash<QString, QImage> originals;
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
    QLabel *m_strengthHint = nullptr;
    QCheckBox *m_upscaleCheck = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QPushButton *m_originalButton = nullptr;
    QSpinBox *m_copiesSpin = nullptr;
    QPushButton *m_printButton = nullptr;
    QPushButton *m_trialButton = nullptr;
    QPushButton *m_backwardButton = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QDoubleSpinBox *m_widthSpin = nullptr;
    QDoubleSpinBox *m_heightSpin = nullptr;
    QDoubleSpinBox *m_angleSpin = nullptr;
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
    QHash<QString, QImage> m_previewImages;
    QHash<QString, QImage> m_originalPreviewImages;
    QPointer<EffectBrowserDialog> m_effectDialog;
    QFutureWatcher<PreviewResult> *m_previewWatcher = nullptr;
    std::shared_ptr<std::stop_source> m_previewStop;
    quint64 m_previewGeneration = 0;
    bool m_previewDirty = false;
    bool m_previewJobActive = false;
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
