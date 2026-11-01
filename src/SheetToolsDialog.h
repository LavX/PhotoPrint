#pragma once

#include "CanvasController.h"
#include "Document.h"
#include "SheetLayout.h"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRectF>

#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;

namespace pp {

/// A dialog-local sheet preview. It paints the paper, the copies it is handed
/// and the guide state as it would print; it never reads the document, so
/// changing a control is free and `Mégsem` leaves no trace.
class SheetPreview : public QWidget
{
    Q_OBJECT

public:
    explicit SheetPreview(QWidget *parent = nullptr);

    void setSheetMm(const SheetLayout::Sheet &sheet);
    /// Rectangles in page millimetres, with one image per rectangle. A null
    /// image paints the placeholder the paper colour provides.
    void setRects(const QList<QRectF> &rectsMm, const QList<QImage> &images);
    void setGuideState(CutGuides guides);
    /// What the preview is showing, for the accessible description.
    QString description() const;

    QSize minimumSizeHint() const override { return QSize(240, 320); }
    QSize sizeHint() const override { return QSize(300, 400); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SheetLayout::Sheet m_sheet;
    QList<QRectF> m_rects;
    QList<QImage> m_images;
    CutGuides m_guides = CutGuides::None;
};

/// The two sheet tools in one window with two tabs: many copies of one
/// photograph, and tidying the photographs already on a page. The dialog owns a
/// copy of the document and only produces an ApplyLayoutEdit on accept, so the
/// window applies one edit and pushes one undo step.
class SheetToolsDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { Batch, Align };

    /// `previews` maps a frame id to its preview layer: either already sRGB or
    /// the canvas's linear layer, which the preview converts once per image.
    SheetToolsDialog(Mode mode, DocumentSnapshot document, int pageIndex,
                     std::optional<Frame> sourceFrame, QHash<QString, QImage> previews,
                     QWidget *parent = nullptr);

    /// The edit the last successful accept() produced.
    ApplyLayoutEdit edit() const { return m_edit; }
    bool hasEdit() const { return m_hasEdit; }
    /// True when the accepted edit replaces the current page's frames, which is
    /// what the window asks about before applying a batch.
    bool replacesCurrentPage() const;

    /// Accepting builds the edit for the active tab; a refused state stays
    /// open. Public because it is the one step a test drives without a modal
    /// event loop.
    void accept() override;

    // Test hooks: the computed result of the current controls, exactly what the
    // preview and the summary are built from. Nothing is applied by reading
    // these.
    std::optional<SheetLayout::BatchGrid> batchGridForTest() const;
    std::optional<SheetLayout::AlignPlan> alignPlanForTest() const;
    std::optional<SheetLayout::DistributePlan> distributePlanForTest() const;
    QString summaryTextForTest() const;
    QString captionTextForTest() const;
    QString warningTextForTest() const;
    bool createEnabledForTest() const;
    bool lastSheetNoteVisibleForTest() const;
    int currentTabIndexForTest() const;

private:
    void buildBatchTab();
    void buildAlignTab();
    void updateBatch();
    void updateAlign();
    void updatePreview();
    void setCreateText();
    void saveSettings() const;

    SheetLayout::BatchSpec batchSpec() const;
    SheetLayout::AlignSpec alignSpec() const;
    SheetLayout::DistributeSpec distributeSpec() const;
    QList<QSize> sourcePixels() const;
    bool buildBatchEdit(ApplyLayoutEdit *edit);
    bool buildAlignEdit(ApplyLayoutEdit *edit);
    /// The frames a batch produces, with the source's slot reference cleared: a
    /// copy is a free frame, and it must not claim a cell of the source page.
    QList<Frame> freeFrames(const QList<Frame> &frames) const;

    DocumentSnapshot m_document;
    int m_pageIndex = 0;
    std::optional<Frame> m_source;
    QHash<QString, QImage> m_previews;
    ApplyLayoutEdit m_edit;
    bool m_hasEdit = false;
    bool m_alignValid = false;
    std::optional<SheetLayout::BatchGrid> m_batchGrid;
    std::optional<SheetLayout::AlignPlan> m_alignPlan;
    std::optional<SheetLayout::DistributePlan> m_distributePlan;
    QList<Frame> m_alignFrames;

    QTabWidget *m_tabs = nullptr;
    SheetPreview *m_preview = nullptr;
    QLabel *m_caption = nullptr;
    QPushButton *m_createButton = nullptr;

    // Batch tab.
    QComboBox *m_batchPreset = nullptr;
    QDoubleSpinBox *m_batchWidthCm = nullptr;
    QDoubleSpinBox *m_batchHeightCm = nullptr;
    QPushButton *m_batchSwap = nullptr;
    QDoubleSpinBox *m_batchGutter = nullptr;
    QDoubleSpinBox *m_batchMargin = nullptr;
    QSpinBox *m_batchCount = nullptr;
    QCheckBox *m_batchFill = nullptr;
    QComboBox *m_batchGuides = nullptr;
    QRadioButton *m_batchCurrent = nullptr;
    QRadioButton *m_batchNew = nullptr;
    QLabel *m_batchPlaceNote = nullptr;
    QLabel *m_batchSummary = nullptr;
    QLabel *m_batchWarning = nullptr;
    QLabel *m_batchLastSheet = nullptr;

    // Align tab.
    QRadioButton *m_alignGridMode = nullptr;
    QRadioButton *m_alignGapMode = nullptr;
    QStackedWidget *m_alignStack = nullptr;
    QDoubleSpinBox *m_alignMargin = nullptr;
    QDoubleSpinBox *m_alignGutter = nullptr;
    QComboBox *m_alignColumns = nullptr;
    QCheckBox *m_alignFill = nullptr;
    QComboBox *m_alignAxis = nullptr;
    QDoubleSpinBox *m_alignGap = nullptr;
    QLabel *m_alignSummary = nullptr;
    QLabel *m_alignWarning = nullptr;
};

} // namespace pp
