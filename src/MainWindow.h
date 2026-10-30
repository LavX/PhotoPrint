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
#include <QImage>
#include <QMainWindow>

#include <QFutureWatcher>

#include <functional>
#include <memory>

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

/// The paper surface. It paints the same page recipe the export uses and adds
/// editor-only marks; the marks never reach the PDF.
class PageView : public QWidget
{
    Q_OBJECT

public:
    explicit PageView(QWidget *parent = nullptr);

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
    QImage composedImage() const;

    std::function<void(const QString &)> onSelectionChanged;
    std::function<void(const Frame &)> onFrameCommitted;
    std::function<void(const QString &)> onDeleteRequested;
    std::function<void()> onCropModeExited;
    std::function<void(const QStringList &)> onFilesDropped;
    std::function<void(double)> onViewZoomChanged;
    std::function<void(bool)> onCropModeChanged;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
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

    const Frame *selectedFramePtr() const;
    QTransform pageToWidget() const;
    QPointF widgetToPage(const QPointF &point) const;
    QPolygonF frameQuadWidget(const Frame &frame) const;
    int hitFrame(const QPointF &widgetPoint) const;
    int hitHandle(const QPointF &widgetPoint) const;
    void beginDrag(DragKind kind, int handle, const QPointF &pagePoint, const Frame &frame);
    void ensureComposed() const;
    void invalidateComposition();
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
    QColor m_canvasColor{0xd9, 0xd9, 0xde};
    QColor m_pageBorderColor{0xc7, 0xc7, 0xcc};
    QColor m_pageShadowColor{0, 0, 0, 38};
    mutable QImage m_composed;
    mutable bool m_composedDirty = true;
    mutable QSize m_composedPixels;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    DocumentSnapshot document() const { return m_controller.document(); }

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

private:
    void buildUi();
    void buildMenus();
    void buildLanguageMenu(QMenu *helpMenu);
    void setLanguage(const QString &code);
    void showAbout();
    void addPhoto(const QString &path);
    void refreshPhotoList();
    void rebuildPreviewImages();
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
    void applyLoadedProject(const LoadedProject &loaded, bool recovery = false);
    void promptMissingSources();
    void checkpointNow();
    void updateTitle();
    Frame makeInitialFrame(const SourceImage &source, const Page &page) const;
    bool pageOverflows() const;
    bool projectPrintGeometryConsistent(const DocumentSnapshot &document) const;
    const Frame *selectedFrame();
    PaperSpec currentPaper() const;
    const PrinterCapabilities *printerByQueue(const QString &queue) const;

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
    QComboBox *m_filterCombo = nullptr;
    QSlider *m_filterStrength = nullptr;
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
