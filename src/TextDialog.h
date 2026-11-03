#pragma once

#include "Document.h"

#include <QDialog>

class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace pp {

/// The one place a text is created or edited. The same dialog serves both: an
/// existing item is passed in and its geometry is kept, so editing never moves
/// the caption. The live preview uses the same block layout and the same glyph
/// painter as the page raster, so what the dialog shows is what prints.
class TextDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TextDialog(const TextItem &item, QWidget *parent = nullptr);

    /// The edited item. The text is normalised ('\r\n' -> '\n') and the geometry
    /// is the one the dialog was built with.
    TextItem item() const;

    /// True when the editor holds nothing but whitespace. The dialog refuses to
    /// accept such a text and says so instead of closing.
    bool textIsEmpty() const;

public slots:
    void accept() override;

private:
    void updatePreview();
    void updateFontHint();
    void chooseColor();
    void syncStyleButtons();

    TextItem m_item;
    QPlainTextEdit *m_editor = nullptr;
    QFontComboBox *m_fontCombo = nullptr;
    QDoubleSpinBox *m_sizeSpin = nullptr;
    QToolButton *m_boldButton = nullptr;
    QToolButton *m_italicButton = nullptr;
    QToolButton *m_underlineButton = nullptr;
    QToolButton *m_alignLeftButton = nullptr;
    QToolButton *m_alignCenterButton = nullptr;
    QToolButton *m_alignRightButton = nullptr;
    QPushButton *m_colorButton = nullptr;
    QDoubleSpinBox *m_spacingSpin = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_fontHint = nullptr;
    QLabel *m_emptyHint = nullptr;
};

} // namespace pp
