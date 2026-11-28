// MagicPopover.swift's MentionField and MentionList: the Magic box, where typing `@` lists what can be pointed at
// (the selection, the playhead, the in/out range, files, clips, markers); arrows move, Return or Tab picks, a click
// picks, Esc closes it. Mentions and times Magic will read are shown in bold.
#pragma once
#include "MagicMentions.h"
#include <QPlainTextEdit>

class QListWidget;

namespace er {

class EditorModel;

struct MentionOption {
    QString id;
    /// What goes in the text after the `@`.
    QString name;
    QString detail;
    bool enabled = true;
    /// `@media:`, `@clip:`, `@marker:`, which list more.
    bool opensList = false;
    std::optional<quint32> tint;
    /// The options for what's typed after the `@`.
    static std::vector<MentionOption> list(const QString& query, const EditorModel& model);
};

class MentionField : public QPlainTextEdit {
    Q_OBJECT
public:
    MentionField(const EditorModel& model, QWidget* parent = nullptr);
    bool listShown() const;
signals:
    /// Return with the list closed (Shift+Return is a new line).
    void submitted();
protected:
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
private:
    void refreshList();
    void pick(int row);
    /// The `@` being typed before the cursor: where it starts, and what follows it.
    std::optional<std::pair<int, QString>> query() const;
    const EditorModel& model_;
    QListWidget* list_;
    std::vector<MentionOption> options_;
    /// An `@` whose list is closed (Esc, or something picked from it): it stays closed while the cursor is in it.
    std::optional<int> dismissed_;
};

} // namespace er
