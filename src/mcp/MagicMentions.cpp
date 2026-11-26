#include "MagicMentions.h"
#include "editor/EditorModel.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace er {

MagicPointers MagicPointers::now(const EditorModel& model) {
    const Project& p = model.project();
    std::vector<Clip> sel;
    for (const auto& c : p.clips) if (model.selection().count(c.id)) sel.push_back(c);
    std::stable_sort(sel.begin(), sel.end(), [&](const Clip& a, const Clip& b) {
        return std::make_pair(a.start, trackOrder(p, a.trackID)) < std::make_pair(b.start, trackOrder(p, b.trackID));
    });
    MagicPointers out;
    out.playhead = model.playhead();
    for (const auto& c : sel) out.selected.push_back(c.id);
    out.range = p.playRange();
    return out;
}

int trackOrder(const Project& p, const Uuid& id) {
    for (size_t i = 0; i < p.videoTracks.size(); ++i) if (p.videoTracks[i].id == id) return int(p.videoTracks.size() - i);
    int a = 0;
    for (size_t i = 0; i < p.audioTracks.size(); ++i) if (p.audioTracks[i].id == id) { a = int(i); break; }
    return int(p.videoTracks.size()) + 1 + a;
}

namespace {
QString tintName(Marker::Tint t) {
    switch (t) {
    case Marker::Tint::white: return QStringLiteral("White");
    case Marker::Tint::yellow: return QStringLiteral("Yellow");
    case Marker::Tint::green: return QStringLiteral("Green");
    case Marker::Tint::blue: return QStringLiteral("Blue");
    case Marker::Tint::purple: return QStringLiteral("Purple");
    case Marker::Tint::red: return QStringLiteral("Red");
    }
    return {};
}
QString bareName(const QString& name) { const int dot = name.lastIndexOf(u'.'); return dot > 0 ? name.left(dot) : name; }
bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == u'_'; }
} // namespace

std::vector<MagicMention::MarkerLabel> MagicMention::markerLabels(const Project& p) {
    std::map<QString, int> seen;
    std::vector<MarkerLabel> out;
    for (const auto& m : p.markerList()) {
        const QString own = m.name.trimmed();
        const QString b = own.isEmpty() ? tintName(m.tint) + QStringLiteral(" marker") : own;
        const int n = ++seen[b.toLower()];
        out.push_back({m.id, n == 1 ? b : QStringLiteral("%1·%2").arg(b).arg(n), m.time, m.tint});
    }
    return out;
}

QString MagicMention::clock(double t) {
    t = std::max(0.0, std::round(t * 100) / 100);
    const int h = int(t) / 3600, m = int(t) % 3600 / 60;
    QString sec = QString::asprintf("%05.2f", std::fmod(t, 60.0));
    while (sec.endsWith(u'0')) sec.chop(1);
    if (sec.endsWith(u'.')) sec.chop(1);
    return h > 0 ? QString::asprintf("%d:%02d:", h, m) + sec : QStringLiteral("%1:").arg(m) + sec;
}

QString MagicMention::markersAsTimes(const QString& text, const Project& p) {
    const auto marks = markerLabels(p);
    if (marks.empty()) return text;
    QString s = text;
    auto found = find(text, p.assets, clipLabels(p), marks);
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        if (it->mention.kind != Kind::marker) continue;
        for (const auto& l : marks) if (l.id == it->mention.id) { s.replace(it->at, it->length, clock(l.time)); break; }
    }
    return s;
}

std::vector<MagicMention::ClipLabel> MagicMention::clipLabels(const Project& p) {
    std::set<Uuid> videoLinks;
    for (const auto& c : p.clips) if (c.kind != ClipKind::audio && c.linkID) videoLinks.insert(*c.linkID);
    std::vector<Clip> clips;
    for (const auto& c : p.clips) if (!(c.kind == ClipKind::audio && c.linkID && videoLinks.count(*c.linkID))) clips.push_back(c);
    std::stable_sort(clips.begin(), clips.end(), [&](const Clip& a, const Clip& b) {
        return std::make_pair(a.start, trackOrder(p, a.trackID)) < std::make_pair(b.start, trackOrder(p, b.trackID));
    });
    auto base = [&](const Clip& c) {
        const QString n = p.name(c).trimmed();
        if (c.kind != ClipKind::text || n.size() <= 24) return n;
        return n.left(22).trimmed() + QStringLiteral("…");
    };
    auto track = [&](const Clip& c) {
        for (size_t i = 0; i < p.videoTracks.size(); ++i) if (p.videoTracks[i].id == c.trackID) return QStringLiteral("V%1").arg(i + 1);
        int a = 0;
        for (size_t i = 0; i < p.audioTracks.size(); ++i) if (p.audioTracks[i].id == c.trackID) { a = int(i); break; }
        return QStringLiteral("A%1").arg(a + 1);
    };
    std::map<QString, int> seen;
    std::vector<ClipLabel> out;
    for (const auto& c : clips) {
        const QString b = base(c);
        const int n = ++seen[b.toLower()];
        out.push_back({c.id, n == 1 ? b : QStringLiteral("%1·%2").arg(b).arg(n), c.kind, track(c), c.start});
    }
    return out;
}

std::vector<MagicMention::Found> MagicMention::find(const QString& s, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips,
                                                    const std::vector<MarkerLabel>& markers) {
    if (!s.contains(u'@')) return {};
    using Name = std::pair<QString, MagicMention>;
    std::vector<Name> names = {{QStringLiteral("playhead"), {Kind::playhead, {}}}, {QStringLiteral("selected"), {Kind::selected, {}}},
                               {QStringLiteral("in-out"), {Kind::range, {}}}};
    for (const auto& a : media) {
        names.push_back({a.name, {Kind::media, a.id}});
        const QString bare = bareName(a.name);
        if (!bare.isEmpty() && bare != a.name) names.push_back({bare, {Kind::media, a.id}});
    }
    std::vector<Name> markerNames;
    for (const auto& m : markers) markerNames.push_back({m.name, {Kind::marker, m.id}});
    // Longest first; earlier first among equals.
    auto longest = [](std::vector<Name> v) {
        std::stable_sort(v.begin(), v.end(), [](const Name& a, const Name& b) { return a.first.size() > b.first.size(); });
        return v;
    };
    names = longest(names);
    std::vector<Name> withMarkers = names;
    withMarkers.insert(withMarkers.end(), markerNames.begin(), markerNames.end());
    withMarkers = longest(withMarkers);
    std::vector<Name> clipNames;
    for (const auto& c : clips) clipNames.push_back({c.name, {Kind::clip, c.id}});
    clipNames = longest(clipNames);
    auto starts = [&](const QString& word, int i) { return i + word.size() <= s.size() && s.mid(i, word.size()).compare(word, Qt::CaseInsensitive) == 0; };
    std::vector<Found> out;
    int i = 0;
    while (i < s.size()) {
        const int at = s.indexOf(u'@', i);
        if (at < 0) break;
        i = at + 1;
        if (at > 0 && isWordChar(s[at - 1])) continue;
        int from = i;
        std::vector<Name> pool = withMarkers;
        if (starts(QStringLiteral("marker:"), i)) {
            from += 7;
            pool = longest(markerNames);
        } else if (starts(QStringLiteral("media:"), i)) {
            from += 6;
            pool.clear();
            for (const auto& n : names) if (n.second.kind == Kind::media) pool.push_back(n);
        } else if (starts(QStringLiteral("clip:"), i)) {
            from += 5;
            pool = clipNames;
        }
        for (const auto& [name, mention] : pool) {
            const int len = int(name.size());
            if (!starts(name, from)) continue;
            if (from + len < s.size() && isWordChar(s[from + len])) continue;
            out.push_back({at, from + len - at, mention});
            i = from + len;
            break;
        }
    }
    return out;
}

QString MagicMention::plain(const QString& text, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips, const std::vector<MarkerLabel>& markers) {
    QString s = text;
    const auto found = find(text, media, clips, markers);
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        QString words = s.mid(it->at, it->length);
        switch (it->mention.kind) {
        case Kind::playhead: words = QStringLiteral("the playhead"); break;
        case Kind::selected: words = QStringLiteral("the selected clips"); break;
        case Kind::range: words = QStringLiteral("the marked range"); break;
        case Kind::media: for (const auto& a : media) if (a.id == it->mention.id) words = a.name; break;
        case Kind::clip: {
            QString n;
            for (const auto& c : clips) if (c.id == it->mention.id) n = c.name;
            words = QStringLiteral("the clip ") + n;
            break;
        }
        case Kind::marker: for (const auto& m : markers) if (m.id == it->mention.id) words = clock(m.time); break;
        }
        s.replace(it->at, it->length, words);
    }
    return s;
}

namespace MagicTimes {

std::vector<Typed> times(const QString& text, double fps) {
    struct Pattern { QString re; std::function<double(const std::vector<double>&)> value; bool unitOnly; };
    const std::vector<Pattern> patterns = {
        {QStringLiteral("(?<![\\d:.])(\\d{1,2}):(\\d{2}):(\\d{2})[;:](\\d{2})(?![\\d:])"),
         [fps](const std::vector<double>& v) { return v[0] * 3600 + v[1] * 60 + v[2] + v[3] / std::max(1.0, fps); }, false},
        {QStringLiteral("(?<![\\d:.])(\\d{1,2}):(\\d{2}):(\\d{2}(?:\\.\\d+)?)(?![\\d:])"),
         [](const std::vector<double>& v) { return v[0] * 3600 + v[1] * 60 + v[2]; }, false},
        {QStringLiteral("(?<![\\d:.])(\\d{1,3}):(\\d{2}(?:\\.\\d+)?)(?![\\d:])"), [](const std::vector<double>& v) { return v[0] * 60 + v[1]; }, false},
        {QStringLiteral("(?<![\\d:.])(\\d+(?:\\.\\d+)?)\\s*(?:s|secs?|seconds?)\\b"), [](const std::vector<double>& v) { return v[0]; }, true},
    };
    std::vector<Typed> out;
    for (const auto& p : patterns) {
        auto it = QRegularExpression(p.re, QRegularExpression::CaseInsensitiveOption).globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const int at = int(m.capturedStart()), len = int(m.capturedLength());
            if (std::any_of(out.begin(), out.end(), [&](const Typed& t) { return at < t.at + t.length && t.at < at + len; })) continue;
            std::vector<double> parts;
            for (int g = 1; g <= m.lastCapturedIndex(); ++g) if (m.capturedStart(g) >= 0) parts.push_back(m.captured(g).toDouble());
            out.push_back({at, len, m.captured(), p.value(parts), p.unitOnly});
        }
    }
    std::sort(out.begin(), out.end(), [](const Typed& a, const Typed& b) { return a.at < b.at; });
    return out;
}

namespace {
const QStringList gaps = {"and", "to", "-", "–", "—", "through", "until", "till"};
}

std::vector<std::pair<int, int>> recognised(const QString& text) {
    const auto found = times(text, 30);
    std::vector<std::pair<int, int>> out;
    for (const auto& t : found) if (!t.unitOnly) out.push_back({t.at, t.length});
    for (size_t k = 0; k + 1 < found.size(); ++k) {
        const auto& a = found[k];
        const auto& b = found[k + 1];
        if (!(a.unitOnly || b.unitOnly)) continue;
        const int gapStart = a.at + a.length;
        if (b.at < gapStart) continue;
        const QString gap = text.mid(gapStart, b.at - gapStart).toLower().trimmed();
        const QString before = text.mid(std::max(0, a.at - 12), std::min(12, a.at)).toLower();
        if (!gaps.contains(gap) || !(before.endsWith(QLatin1String("between ")) || before.endsWith(QLatin1String("from ")))) continue;
        out.push_back({a.at, a.length});
        out.push_back({b.at, b.length});
    }
    return out;
}

std::optional<Span> span(const QString& text, double fps) {
    const auto found = times(text, fps);
    for (size_t k = 0; k + 1 < found.size(); ++k) {
        const auto& a = found[k];
        const auto& b = found[k + 1];
        const int gapStart = a.at + a.length;
        if (b.at < gapStart) continue;
        const QString gap = text.mid(gapStart, b.at - gapStart).toLower().trimmed();
        if (!gaps.contains(gap)) continue;
        const QString before = text.mid(std::max(0, a.at - 12), std::min(12, a.at)).toLower();
        const bool introduced = before.endsWith(QLatin1String("between ")) || before.endsWith(QLatin1String("from "));
        if (!(introduced || (!a.unitOnly && !b.unitOnly)) || !(b.seconds > a.seconds)) continue;
        const int from = introduced ? (before.endsWith(QLatin1String("between ")) ? 8 : 5) : 0;
        const int start = a.at - from;
        return Span{a.seconds, b.seconds, text.mid(start, b.at + b.length - start)};
    }
    return std::nullopt;
}

} // namespace MagicTimes
} // namespace er
