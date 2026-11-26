#include "MagicPopover.h"
#include "LocalMagic.h"
#include "MentionField.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

namespace er {

MagicPopover::MagicPopover(Controller& c, MagicAgent& agent, QWidget* parent) : QDialog(parent), c_(c), agent_(agent) {
    setWindowTitle(QStringLiteral("Magic"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 16, 18, 16);
    auto* row = new QHBoxLayout;
    tabs_ = new QButtonGroup(this);
    const QString saved = QSettings().value(QStringLiteral("magicTab"), QStringLiteral("claude")).toString();
    for (const auto& [raw, label] : std::vector<std::pair<QString, QString>>{{"claude", "Claude"}, {"chatgpt", "ChatGPT"}, {"local", "Local"}}) {
        auto* b = new QRadioButton(label);
        b->setProperty("raw", raw);
        b->setChecked(raw == saved);
        tabs_->addButton(b);
        row->addWidget(b);
    }
    row->addStretch();
    v->addLayout(row);
    request_ = new MentionField(c_.model());
    request_->setFixedHeight(90);
    v->addWidget(request_);
    status_ = new QLabel;
    status_->setFont(Theme::font(11.5, 500));
    v->addWidget(status_);
    note_ = new QLabel;
    note_->setWordWrap(true);
    note_->setFont(Theme::font(11.5));
    v->addWidget(note_);
    auto* foot = new QHBoxLayout;
    foot->addStretch();
    go_ = new PillButton(QStringLiteral("Make"), true);
    foot->addWidget(go_);
    v->addLayout(foot);
    setMinimumWidth(460);
    connect(static_cast<MentionField*>(request_), &MentionField::submitted, go_, &QPushButton::click);
    connect(tabs_, &QButtonGroup::buttonClicked, this, [this] { QSettings().setValue(QStringLiteral("magicTab"), tab()); refresh(); });
    connect(go_, &QPushButton::clicked, this, [this] {
        if (agent_.running()) { agent_.stop(); return; }
        note_->clear();
        const QString t = tab();
        if (t == QLatin1String("local")) {
            LocalMagic& local = LocalMagic::of(c_);
            if (local.busy()) { local.stop(); refresh(); return; }
            c_.model().addMagicPrompt(request_->toPlainText(), t);
            local.run(request_->toPlainText());
            refresh();
            return;
        }
        c_.model().addMagicPrompt(request_->toPlainText(), t);
        agent_.start(request_->toPlainText(), t == QLatin1String("claude") ? MagicAgent::Kind::claude : MagicAgent::Kind::chatgpt);
        refresh();
    });
    connect(&agent_, &MagicAgent::statusChanged, this, &MagicPopover::refresh);
    connect(&LocalMagic::of(c_), &LocalMagic::statusChanged, this, &MagicPopover::refresh);
    connect(&LocalMagic::of(c_), &LocalMagic::finished, this, [this](const QString& said, bool) {
        note_->setText(said);
        refresh();
    });
    connect(&agent_, &MagicAgent::finished, this, [this](const QString& said, bool changed, const QString& failure) {
        const QString who = tab() == QLatin1String("chatgpt") ? QStringLiteral("ChatGPT") : QStringLiteral("Claude");
        note_->setText(failure.isEmpty() ? said : QStringLiteral("%1 couldn't make the edit. %2").arg(who, failure));
        (void)changed;
        refresh();
    });
    refresh();
}

QString MagicPopover::tab() const {
    auto* b = tabs_->checkedButton();
    return b ? b->property("raw").toString() : QStringLiteral("claude");
}

void MagicPopover::refresh() {
    const bool empty = c_.model().project().clips.empty();
    const QString t = tab();
    const QString name = t == QLatin1String("chatgpt") ? QStringLiteral("ChatGPT") : QStringLiteral("Claude");
    if (t == QLatin1String("local"))
        request_->setPlaceholderText(empty ? QStringLiteral("Describe the video you want (length, pace, look, music, vertical), or leave this empty for the best cut.")
                                           : QStringLiteral("Describe what to change (length, pace, look, music, vertical), or leave this empty to improve the edit."));
    else
        request_->setPlaceholderText(empty ? QStringLiteral("Describe the video you want, or leave this empty and %1 will make the best video it can from the clips and music in your Media.").arg(name)
                                           : QStringLiteral("Describe what to change, or leave this empty and %1 will make the best video it can from your Media.").arg(name));
    const LocalMagic& local = LocalMagic::of(c_);
    status_->setText(t == QLatin1String("local") ? local.status() : agent_.status());
    go_->setText((t == QLatin1String("local") ? local.busy() : agent_.running()) ? QStringLiteral("Stop") : QStringLiteral("Make"));
}

} // namespace er
