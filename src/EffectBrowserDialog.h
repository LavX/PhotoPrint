#pragma once

#include "EffectCatalogue.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSize>
#include <QStringList>

#include <memory>
#include <stop_token>

class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;

namespace pp {

class EffectTile;

/// Everything the browser needs to open without touching the document again:
/// the frame's current effect, the strength it uses, and one small sRGB sample
/// of the selected photo (or the bundled sample when nothing is selected).
struct EffectBrowserRequest {
    QString currentKey = QStringLiteral("original");
    int currentVersion = 0;
    int currentStrength = 100;
    QImage sample;
    /// Identity of the sample pixels. Part of the thumbnail cache key, so a
    /// replaced photo can never show a stale tile.
    QString sampleHash;
    /// Logical side of one tile. The raster is capped at 240 device pixels.
    int tileLogicalPx = 150;
};

/// The button label for a frame's effect key. `Eredeti` for the untouched
/// frame, the recipe's single product name for a catalogue effect, and the
/// frozen legacy label for a 1.0.x preset. Legacy keys never grow.
QString effectButtonLabel(const QString &key, int version = 0);

/// Legacy preset keys are the frozen 1.0.x PhotoFilters path, not recipes.
QString legacyEffectLabel();
QString newEffectAffordance();

/// True for the recipes whose dots or grain are finish stages: strength is not
/// a continuous control there, because the screen either exists or it does not.
bool effectIgnoresStrength(const QString &key);
QString effectStrengthReason();

/// Effect browser: the twelve categories on the left, an `Eredeti` tile at the
/// head of the grid, a search field and a strength slider. Tiles are rendered
/// lazily on one worker with a generation token, so the dialog opens at once
/// and only the visible category costs anything.
class EffectBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EffectBrowserDialog(const EffectBrowserRequest &request, QWidget *parent = nullptr);
    ~EffectBrowserDialog() override;

    // Inspection hooks for the tests. They describe what the user can see, not
    // an internal cache.
    int categoryCount() const;
    int visibleTileCount() const;
    QStringList visibleTileKeys() const;
    QString tileLabelFor(const QString &key) const;
    bool tileHasImage(const QString &key) const;
    int renderedTileCount() const;
    bool strengthEnabled() const;
    QString strengthHintText() const;
    QString adjacentHintText() const;
    QString currentKey() const;
    QString selectedKey() const;
    bool tileMarkedCurrent(const QString &key) const;
    int strengthValue() const;
    QSlider *strengthSlider() const { return m_strength; }
    /// Processes events until the visible set is rendered. Returns false on
    /// timeout; the dialog is never blocked while it waits.
    bool waitForRendering(int timeoutMs = 5000);

signals:
    /// A tile was applied with Apply, Enter or a double click. The version is
    /// the recipe's version and 0 for `Eredeti`.
    void effectChosen(const QString &key, int version, int strength);
    /// The strength slider moved. The frame the browser was opened for is the
    /// one being edited, live.
    void strengthEdited(int strength);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct RenderJob {
        quint64 generation = 0;
        QList<EffectRecipe> recipes;
        QImage sample;
        QSize tileSize;
        int rasterPx = 0;
        int strength = 100;
        QString sampleHash;
        std::shared_ptr<std::stop_source> stop;
    };

    struct RenderResult {
        quint64 generation = 0;
        QHash<QString, QImage> images;
    };

    /// Runs on the worker. It touches only the job, never the dialog.
    static RenderResult renderTiles(RenderJob job);

    void buildUi();
    void fillCategories();
    void rebuildGrid();
    void relayoutGrid();
    int columnCount() const;
    void scheduleRender();
    void handleRenderFinished();
    void selectTile(const QString &key);
    void activateCurrent();
    void syncStrengthUi();
    void updateCountLabel();
    void moveFocus(int delta);
    QString cacheKeyFor(const QString &key) const;
    QString scaledSampleHash() const;

    EffectBrowserRequest m_request;
    QList<EffectRecipe> m_recipes;
    QHash<QString, EffectRecipe> m_byKey;

    QLineEdit *m_search = nullptr;
    QListWidget *m_categories = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_gridHost = nullptr;
    QGridLayout *m_grid = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_strengthLabel = nullptr;
    QLabel *m_strengthValue = nullptr;
    QLabel *m_strengthHint = nullptr;
    QLabel *m_adjacentHint = nullptr;
    QSlider *m_strength = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    QHash<QString, EffectTile *> m_tiles;
    QStringList m_visibleKeys;
    QString m_currentKey;  ///< the effect the frame uses, marked with a check
    QString m_selectedKey; ///< the highlighted tile, the target of Apply
    QImage m_sample;
    QSize m_tileSize;
    int m_rasterPx = 0;
    int m_strengthMemory = 100;
    int m_columns = 5;
    bool m_started = false;

    QHash<QString, QImage> m_images; ///< GUI-thread thumbnail store
    quint64 m_generation = 0;
    QFutureWatcher<RenderResult> *m_watcher = nullptr;
    std::shared_ptr<RenderJob> m_activeJob;
    bool m_rendering = false;
};

} // namespace pp
