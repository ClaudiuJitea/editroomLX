// MarkerPanel (TimelineCanvas.swift): the panel a marker's flag opens, above the flag: its name and colour, a
// button to delete it, and every marker to jump to.
#pragma once
#include "editor/EditorModel.h"
#include <QFrame>

class QLineEdit;
class QLabel;
class QVBoxLayout;

namespace er {

class MarkerPanel : public QFrame {
    Q_OBJECT
public:
    MarkerPanel(EditorModel& m, QWidget* parent);
    void setMarker(const Uuid& id);
    /// Opens with its bottom edge just above `flag` (global), centred on it, kept on screen.
    void showAbove(QPoint flag);
signals:
    void closed();
protected:
    void closeEvent(QCloseEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void refresh();
    std::optional<Marker> marker() const;
    EditorModel& m_;
    Uuid id_;
    QLabel* time_;
    QLineEdit* name_;
    QWidget* tints_;
    QWidget* list_;
    QVBoxLayout* rows_;
    bool closing_ = false;
};

} // namespace er
