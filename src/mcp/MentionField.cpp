#include "MentionField.h"
#include "editor/EditorModel.h"
#include "ui/Theme.h"
#include <QKeyEvent>
#include <QListWidget>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextLayout>

namespace er {

namespace {
QString clockOf(double t) {
    const double s = std::max(0.0, t);
    return QString::asprintf("%d:%04.1f", int(s) / 60, std::fmod(s, 60.0));
}

MentionOption fileOption(const MediaAsset& a) {
    const int s = int(std::round(a.duration));
    const QString length = QString::asprintf("%d:%02d", s / 60, s % 60);
    const QString detail = a.kind == MediaKind::video ? length + QStringLiteral("  ·  Video")
                         : a.kind == MediaKind::audio ? length + QStringLiteral("  ·  Audio") : QStringLiteral("Photo");
    return {a.id.toString(QUuid::WithoutBraces), a.name, detail};
}

std::vector<MentionOption> files(const QString& text, const std::vector<MediaAsset>& assets) {
    std::vector<MentionOption> out;
    if (text.isEmpty()) { for (const auto& a : assets) out.push_back(fileOption(a)); return out; }
    for (const auto& a : assets) if (a.name.toLower().startsWith(text)) out.push_back(fileOption(a));
    if (!text.contains(u' '))
        for (const auto& a : assets) if (!a.name.toLower().startsWith(text) && a.name.toLower().contains(text)) out.push_back(fileOption(a));
    return out;
}

/// Mentions and times Magic will read, in bold.
class MentionHighlighter : public QSyntaxHighlighter {
public:
    MentionHighlighter(QTextDocument* doc, const EditorModel& m) : QSyntaxHighlighter(doc), model_(m) {}
protected:
    void highlightBlock(const QString& text) override {
        QTextCharFormat bold;
        bold.setFontWeight(QFont::DemiBold);
        const Project& p = model_.project();
        for (const auto& f : MagicMention::find(text, p.assets, MagicMention::clipLabels(p), MagicMention::markerLabels(p))) setFormat(f.at, f.length, bold);
        for (const auto& [at, len] : MagicTimes::recognised(text)) setFormat(at, len, bold);
    }
private:
    const EditorModel& model_;
};
} // namespace

std::vector<MentionOption> MentionOption::list(const QString& query, const EditorModel& model) {
    const Project& p = model.project();
    const QString q = query.toLower();
    static const std::map<QString, MediaKind> kinds = {{"video", MediaKind::video}, {"videos", MediaKind::video}, {"audio", MediaKind::audio},
        {"sound", MediaKind::audio}, {"music", MediaKind::audio}, {"photo", MediaKind::image}, {"photos", MediaKind::image}, {"image", MediaKind::image},
        {"images", MediaKind::image}};
    if (q.startsWith(QLatin1String("clip:"))) {
        const QString rest = q.mid(5);
        static const std::map<QString, std::set<ClipKind>> clipKinds = {{"text", {ClipKind::text}}, {"texts", {ClipKind::text}}, {"title", {ClipKind::text}},
            {"titles", {ClipKind::text}}, {"solid", {ClipKind::solid}}, {"solids", {ClipKind::solid}}, {"video", {ClipKind::video}}, {"videos", {ClipKind::video}},
            {"audio", {ClipKind::audio}}, {"sound", {ClipKind::audio}}, {"music", {ClipKind::audio}}, {"photo", {ClipKind::image}}, {"photos", {ClipKind::image}},
            {"image", {ClipKind::image}}};
        auto row = [](const MagicMention::ClipLabel& l) {
            const QString kind = l.kind == ClipKind::video ? QStringLiteral("Video") : l.kind == ClipKind::audio ? QStringLiteral("Audio")
                               : l.kind == ClipKind::image ? QStringLiteral("Photo") : l.kind == ClipKind::solid ? QStringLiteral("Solid") : QStringLiteral("Text");
            return MentionOption{l.id.toString(QUuid::WithoutBraces), QStringLiteral("clip:") + l.name,
                                 QStringLiteral("%1  ·  %2  ·  %3").arg(l.track, clockOf(l.start), kind)};
        };
        std::vector<MentionOption> out;
        const auto labels = MagicMention::clipLabels(p);
        if (auto it = clipKinds.find(rest); it != clipKinds.end()) {
            for (const auto& l : labels) if (it->second.count(l.kind)) out.push_back(row(l));
            return out;
        }
        for (const auto& l : labels) if (l.name.toLower().startsWith(rest)) out.push_back(row(l));
        if (!rest.isEmpty() && !rest.contains(u' '))
            for (const auto& l : labels) if (!l.name.toLower().startsWith(rest) && l.name.toLower().contains(rest)) out.push_back(row(l));
        return out;
    }
    const auto markers = MagicMention::markerLabels(p);
    auto mark = [](const MagicMention::MarkerLabel& l) {
        return MentionOption{l.id.toString(QUuid::WithoutBraces), QStringLiteral("marker:") + l.name, QStringLiteral("Marker  ·  ") + clockOf(l.time), true, false,
                             Marker::hex(l.tint)};
    };
    if (q.startsWith(QLatin1String("marker:"))) {
        const QString rest = q.mid(7);
        std::vector<MentionOption> out;
        for (const auto& l : markers) if (l.name.toLower().startsWith(rest)) out.push_back(mark(l));
        if (!rest.isEmpty() && !rest.contains(u' '))
            for (const auto& l : markers) if (!l.name.toLower().startsWith(rest) && l.name.toLower().contains(rest)) out.push_back(mark(l));
        return out;
    }
    if (q.startsWith(QLatin1String("media:"))) {
        const QString rest = q.mid(6);
        if (auto it = kinds.find(rest); it != kinds.end()) {
            std::vector<MentionOption> out;
            for (const auto& a : p.assets) if (a.kind == it->second) out.push_back(fileOption(a));
            return out;
        }
        return files(rest, p.assets);
    }
    int selected = 0;
    for (const auto& c : p.clips) {
        if (!model.selection().count(c.id)) continue;
        // A video's own sound selected with it isn't another clip.
        bool partner = false;
        if (c.kind == ClipKind::audio && c.linkID)
            for (const auto& o : p.clips) if (o.linkID == c.linkID && o.id != c.id && model.selection().count(o.id)) partner = true;
        if (!partner) ++selected;
    }
    std::vector<MentionOption> pointers;
    if (selected > 0) pointers.push_back({QStringLiteral("selected"), QStringLiteral("selected"), QStringLiteral("%1 clip%2").arg(selected).arg(selected == 1 ? QString() : QStringLiteral("s"))});
    else pointers.push_back({QStringLiteral("selected"), QStringLiteral("selected"), QStringLiteral("Select clips first"), false});
    pointers.push_back({QStringLiteral("playhead"), QStringLiteral("playhead"), clockOf(model.playhead())});
    if (auto r = p.playRange()) pointers.push_back({QStringLiteral("in-out"), QStringLiteral("in-out"), clockOf(r->start) + QStringLiteral(" to ") + clockOf(r->end)});
    if (!p.assets.empty()) {
        const int n = int(p.assets.size());
        pointers.push_back({QStringLiteral("media"), QStringLiteral("media:"), QStringLiteral("%1 file%2  ›").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")), true, true});
    }
    const int onTimeline = int(MagicMention::clipLabels(p).size());
    if (onTimeline > 0) pointers.push_back({QStringLiteral("clip"), QStringLiteral("clip:"), QStringLiteral("%1 on the timeline  ›").arg(onTimeline), true, true});
    if (!markers.empty()) pointers.push_back({QStringLiteral("marker"), QStringLiteral("marker:"), QStringLiteral("%1 on the timeline  ›").arg(markers.size()), true, true});
    std::vector<MentionOption> out;
    for (const auto& o : pointers) if (o.name.startsWith(q)) out.push_back(o);
    if (q.isEmpty()) return out;
    for (const auto& l : markers) if (l.name.toLower().startsWith(q)) out.push_back(mark(l));
    for (auto& f : files(q, p.assets)) out.push_back(f);
    return out;
}

MentionField::MentionField(const EditorModel& model, QWidget* parent) : QPlainTextEdit(parent), model_(model) {
    new MentionHighlighter(document(), model);
    list_ = new QListWidget(this);
    list_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setFont(Theme::font(12));
    list_->hide();
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { pick(list_->row(it)); });
    connect(this, &QPlainTextEdit::textChanged, this, &MentionField::refreshList);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &MentionField::refreshList);
}

bool MentionField::listShown() const { return list_->isVisible(); }

std::optional<std::pair<int, QString>> MentionField::query() const {
    const QTextCursor c = textCursor();
    const QString text = c.block().text();
    const int pos = c.positionInBlock();
    const int at = text.lastIndexOf(u'@', pos - 1);
    if (at < 0) return std::nullopt;
    if (at > 0 && (text[at - 1].isLetterOrNumber() || text[at - 1] == u'_')) return std::nullopt;
    const QString q = text.mid(at + 1, pos - at - 1);
    // A finished mention, or words long past it, closes the list.
    if (q.size() > 40 || q.contains(QLatin1String("  "))) return std::nullopt;
    return std::make_pair(c.block().position() + at, q);
}

void MentionField::refreshList() {
    // Mid-composition (Pinyin, Japanese, a dead key): the input method owns the text until a choice is made,
    // so the candidate list stays. Plain typing never composes, so mentions still update on every key.
    if (const QTextLayout* l = textCursor().block().layout(); l && !l->preeditAreaText().isEmpty()) return;
    const auto q = query();
    options_ = q ? MentionOption::list(q->second, model_) : std::vector<MentionOption>{};
    // Exactly what's typed already: nothing to offer.
    if (options_.size() == 1 && q && options_[0].name.compare(q->second, Qt::CaseInsensitive) == 0 && !options_[0].opensList) options_.clear();
    if (options_.empty() || !hasFocus()) { list_->hide(); return; }
    list_->clear();
    for (const auto& o : options_) {
        auto* it = new QListWidgetItem(QStringLiteral("@%1    %2").arg(o.name, o.detail), list_);
        if (!o.enabled) it->setFlags(Qt::NoItemFlags);
        if (o.tint) it->setForeground(QColor::fromRgb(*o.tint));
    }
    int first = 0;
    while (first < int(options_.size()) && !options_[size_t(first)].enabled) ++first;
    list_->setCurrentRow(std::min(first, int(options_.size()) - 1));
    const QRect cr = cursorRect();
    const int rows = std::min(int(options_.size()), 7);
    list_->setFixedSize(std::max(260, width()), rows * 25 + 6);
    list_->move(mapToGlobal(QPoint(0, cr.bottom() + 4)));
    list_->show();
}

void MentionField::pick(int row) {
    const auto q = query();
    if (!q || row < 0 || row >= int(options_.size()) || !options_[size_t(row)].enabled) return;
    const MentionOption o = options_[size_t(row)];
    QTextCursor c = textCursor();
    c.setPosition(q->first);
    c.setPosition(q->first + 1 + int(q->second.size()), QTextCursor::KeepAnchor);
    c.insertText(u'@' + o.name + (o.opensList ? QString() : QStringLiteral(" ")));
    setTextCursor(c);
    refreshList();
}

void MentionField::keyPressEvent(QKeyEvent* e) {
    if (list_->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down: {
            int r = list_->currentRow();
            const int step = e->key() == Qt::Key_Up ? -1 : 1;
            for (int i = 0; i < int(options_.size()); ++i) {
                r = (r + step + int(options_.size())) % int(options_.size());
                if (options_[size_t(r)].enabled) break;
            }
            list_->setCurrentRow(r);
            return;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            pick(list_->currentRow());
            return;
        case Qt::Key_Escape:
            options_.clear();
            list_->hide();
            return;
        default: break;
        }
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
        emit submitted();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void MentionField::focusOutEvent(QFocusEvent* e) {
    list_->hide();
    QPlainTextEdit::focusOutEvent(e);
}

} // namespace er
