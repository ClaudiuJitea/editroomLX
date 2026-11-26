// EditroomAlert: a message, details and buttons in the Mac's order (the first is the default, on the
// right; the others to its left), styled with the theme.
#pragma once
#include <QDialog>
#include <QStringList>

namespace er {

class Alert : public QDialog {
    Q_OBJECT
public:
    Alert(const QString& message, const QString& info, const QStringList& buttons, QWidget* parent = nullptr);
    /// The index of the button pressed (Escape: the one named "Cancel"/"Not Now", or the last).
    static int run(QWidget* parent, const QString& message, const QString& info, const QStringList& buttons);
    int choice() const { return choice_; }
private:
    int choice_ = -1;
};

} // namespace er
