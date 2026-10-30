#pragma once

#include <QDialog>

class QPushButton;

namespace pp {

/// Modal About window: the application icon, the version from CMake, the
/// licence and the third-party credits. Every label carries an accessible name
/// so a screen reader can walk it in order.
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    /// The version string this build was compiled with, without decorations.
    static QString versionString();

    QPushButton *closeButton() const { return m_closeButton; }

private:
    QPushButton *m_closeButton = nullptr;
};

} // namespace pp
