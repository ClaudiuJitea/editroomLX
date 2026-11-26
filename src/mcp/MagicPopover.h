// MagicPopover.swift: the Magic button's panel. Claude, ChatGPT or Local; what to make or change; Make;
// while it works, who's doing what and Stop; then what the AI said.
#pragma once
#include "MagicAgent.h"
#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QButtonGroup;

namespace er {

class MagicPopover : public QDialog {
    Q_OBJECT
public:
    MagicPopover(Controller& c, MagicAgent& agent, QWidget* parent = nullptr);
private:
    void refresh();
    QString tab() const;
    Controller& c_;
    MagicAgent& agent_;
    QPlainTextEdit* request_;
    QLabel* status_;
    QLabel* note_;
    QPushButton* go_;
    QButtonGroup* tabs_;
};

} // namespace er
