#include "LocalPlanners.h"
#include "LocalImprover.h"
#include "model/Captions.h"
#include <algorithm>
#include <cmath>

namespace er {

namespace {
bool any(const QString& t, std::initializer_list<const char*> words) {
    for (const char* w : words) if (t.contains(QLatin1String(w))) return true;
    return false;
}
MagicEdit kindEdit(MagicEdit::Kind k) { MagicEdit e; e.kind = k; return e; }
MagicEdit removeEdit(const IdSet& ids, bool ripple = false) {
    MagicEdit e = kindEdit(MagicEdit::Kind::remove);
    e.ids.assign(ids.begin(), ids.end());
    e.ripple = ripple;
    return e;
}
MagicEdit moveEdit(const std::vector<Uuid>& ids, double from, double to) {
    MagicEdit e = kindEdit(MagicEdit::Kind::move);
    e.ids = ids;
    e.from = from;
    e.to = to;
    return e;
}
MagicEdit bladeEdit(const std::vector<Uuid>& ids, double at, const std::map<Uuid, Uuid>& pieces) {
    MagicEdit e = kindEdit(MagicEdit::Kind::blade);
    e.ids = ids;
    e.at = at;
    e.pieces = pieces;
    e.link = newUuid();
    return e;
}
MagicEdit fadeEdit(bool in, const Uuid& id, double to) { MagicEdit e = kindEdit(in ? MagicEdit::Kind::fadeIn : MagicEdit::Kind::fadeOut); e.clip = id; e.to = to; return e; }
MagicEdit typeEdit(const Uuid& id, const QString& text) { MagicEdit e = kindEdit(MagicEdit::Kind::type); e.clip = id; e.text = text; return e; }
MagicEdit lookEdit(const Uuid& id, const QString& name) { MagicEdit e = kindEdit(MagicEdit::Kind::look); e.clip = id; e.text = name; return e; }
LocalOutcome said(const QString& s) { return {std::nullopt, s}; }
LocalOutcome planned(const Project& p, const std::vector<MagicEdit>& edits, const QString& s) { return {LocalImprover::sequential(p, edits), s}; }
QString capitalizedFirst(const QString& s) { return s.left(1).toUpper() + s.mid(1); }
QString plural(int n, const QString& word) { return QStringLiteral("%1 %2%3").arg(n).arg(word, n == 1 ? QString() : QStringLiteral("s")); }
QString g(double v) { return QString::number(v, 'g', 6); }
bool visual(const Clip& c) { return c.kind == ClipKind::video || c.kind == ClipKind::image; }
QString bareName(const QString& name) { const int dot = name.lastIndexOf(u'.'); return dot > 0 ? name.left(dot) : name; }
IdSet allIds(const Project& q) { IdSet s; for (const auto& c : q.clips) s.insert(c.id); return s; }
} // namespace

// MARK: LocalTitle

namespace LocalTitle {

bool newCut(const QString& request) {
    const QString t = request.toLower();
    return any(t, {"new cut", "new edit", "new video", "montage", "recut", "re-cut", "from scratch", "start over"}) || LocalBrain::wantsLibrary(request)
        || (any(t, {"make a", "make an", "make me", "create", "build a", "cut a", "edit a"})
            && any(t, {"video", "cut", "edit", "montage", "reel", "recap", "vlog", "movie", "film"}));
}

bool asks(const QString& request, const Project& p) {
    const bool hasEdit = std::any_of(p.clips.begin(), p.clips.end(), visual);
    return hasEdit && !newCut(request) && LocalBrain::titleAsked(request).has_value();
}

LocalOutcome plan(const QString& request, double playhead, const Project& p) {
    double first = INFINITY, last = -INFINITY;
    for (const auto& c : p.clips) if (visual(c)) { first = std::min(first, c.start); last = std::max(last, c.end()); }
    if (!std::isfinite(first)) return said(QStringLiteral("There's no edit on the timeline to put a title on."));
    enum class Spot { start, end, playhead, at } where = Spot::start;
    double typedAt = 0;
    const QString t = request.toLower();
    std::optional<MagicTimes::Typed> typed;
    if (!MagicTimes::span(request, p.fps)) for (const auto& x : MagicTimes::times(request, p.fps)) if (!x.unitOnly) { typed = x; break; }
    if (typed) { where = Spot::at; typedAt = typed->seconds; }
    else if (LocalBrain::titleAtEnd(request)) where = Spot::end;
    else if (any(t, {"playhead", "here", "right now", "this point"})) where = Spot::playhead;
    const double length = std::min(3.0, std::max(1.0, (last - first) * 0.25));
    double start = first;
    switch (where) {
    case Spot::start: start = first; break;
    case Spot::end: start = std::max(first, last - length); break;
    case Spot::playhead: start = std::min(std::max(first, playhead), std::max(first, last - length)); break;
    case Spot::at: start = std::min(std::max(first, typedAt), std::max(first, last - length)); break;
    }
    QString words = LocalBrain::titleAsked(request).value_or(QString());
    if (words.isEmpty()) words = QStringLiteral("Title");
    std::vector<MagicEdit> edits;
    const double s = p.snapFrame(start), d = p.snapFrame(length);
    Uuid track;
    const Track* free = nullptr;
    for (size_t i = 1; i < p.videoTracks.size(); ++i) {
        const Track& tr = p.videoTracks[i];
        if (tr.locked) continue;
        if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == tr.id && c.start < s + d && c.end() > s; })) continue;
        free = &tr;
        break;
    }
    if (free) {
        track = free->id;
    } else {
        track = newUuid();
        const Uuid id = track;
        edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Add video track"), [id](Project& q) {
            Track tr(TrackKind::video);
            tr.id = id;
            q.videoTracks.push_back(tr);
        }));
    }
    Clip clip(track, ClipKind::text, s, d);
    clip.text.string = QString();
    clip.text.size = 130;
    edits.push_back(MagicEdit::place({clip}, std::nullopt));
    edits.push_back(typeEdit(clip.id, words));
    edits.push_back(fadeEdit(true, clip.id, 0.4));
    edits.push_back(fadeEdit(false, clip.id, 0.5));
    QString place;
    switch (where) {
    case Spot::end: place = QStringLiteral("over the last %1 seconds").arg(int(std::round(length))); break;
    case Spot::start: place = QStringLiteral("at the start"); break;
    case Spot::playhead: place = QStringLiteral("at the playhead"); break;
    case Spot::at: place = QStringLiteral("at ") + MagicMention::clock(start); break;
    }
    return planned(p, edits, QStringLiteral("Added a title, “%1”, %2.").arg(words, place));
}

} // namespace LocalTitle

// MARK: LocalCaptions

namespace LocalCaptions {

bool asks(const QString& request, const Project& p) {
    const QString t = request.toLower();
    if (!any(t, {"caption", "subtitle", "closed caption", "transcribe the", "words on screen as they talk"})) return false;
    if (LocalBrain::quotedWords(request)) return false;
    if (LocalTitle::newCut(request)) return false;
    return LocalBrain::captionsRemoved(request) || std::any_of(p.clips.begin(), p.clips.end(), [](const Clip& c) { return c.kind == ClipKind::audio; });
}

IdSet targets(const MagicPointers& ptr, const Project& p) {
    const IdSet picked = p.captionTargets(IdSet(ptr.selected.begin(), ptr.selected.end()));
    return p.unlocked(picked.empty() ? p.defaultCaptionTargets() : picked);
}

LocalOutcome plan(const QString& request, const MagicPointers& ptr, const Project& p, const TranscriptOf& transcript) {
    if (LocalBrain::captionsRemoved(request)) {
        if (!p.captions) return said(QStringLiteral("There are no captions to remove."));
        return planned(p, {MagicEdit::change(std::nullopt, QStringLiteral("No captions"), [](Project& q) { q.removeCaptionTrack(); })},
                       QStringLiteral("Removed the captions."));
    }
    const IdSet t = targets(ptr, p);
    if (t.empty()) return said(QStringLiteral("There's no one talking to caption. Pick a clip with sound and ask again."));
    Project after = p;
    after.addCaptions(t);
    const int count = int(CaptionLayout::blocks(after, [&](const MediaAsset& a) { return transcript(a.id); }).size());
    const QString s = count == 0
        ? QStringLiteral("Added a caption track, but no speech was heard in %1.").arg(t.size() == 1 ? QStringLiteral("that clip") : QStringLiteral("those clips"))
        : QStringLiteral("Captioned what's said: %1 on the caption track.").arg(plural(count, QStringLiteral("caption")));
    return planned(p, {MagicEdit::change(*t.begin(), QStringLiteral("Captions"), [t](Project& q) { q.addCaptions(t); })}, s);
}

} // namespace LocalCaptions

// MARK: LocalAdd

namespace LocalAdd {

bool asks(const QString& request, const Project& p) {
    const QString t = u' ' + request.toLower() + u' ';
    const bool verb = any(t, {" add ", " put ", " append ", " insert ", " stick ", " throw in ", " include "});
    const bool thing = any(t, {" clip", " shot", " video", " photo", " picture", " image"});
    const bool more = any(t, {" more ", " another ", " extra ", " end", " start", " beginning", " begin", " front", " last", " first", " after ", " before ",
                              " between ", "playhead", "selected", " here", "@"});
    const bool hasEdit = std::any_of(p.clips.begin(), p.clips.end(), visual);
    const bool fresh = any(t, {"new cut", "new edit", "new video", "montage", "recut", "re-cut", "from scratch", "start over", "make a", "make an", "make me",
                               "create", "build a"});
    return verb && thing && more && hasEdit && !fresh;
}

namespace {
QStringList wordsOf(const QString& s) {
    QStringList out;
    QString cur;
    for (QChar c : s) {
        if (c.isLetterOrNumber()) cur += c;
        else if (!cur.isEmpty()) { out << cur; cur.clear(); }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}
} // namespace

int count(const QString& request) {
    const QStringList words = wordsOf(request.toLower());
    static const std::map<QString, int> named = {{"one", 1}, {"a", 1}, {"an", 1}, {"another", 1}, {"two", 2}, {"couple", 2}, {"three", 3}, {"few", 3},
                                                 {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}};
    static const QStringList things = {"clip", "clips", "shot", "shots", "video", "videos", "photo", "photos", "more"};
    for (int i = 0; i < words.size(); ++i) {
        bool isNum = false;
        const int n = words[i].toInt(&isNum);
        if (!(i + 1 < words.size() || isNum)) continue;
        const QStringList rest = words.mid(i + 1, 3);
        if (!std::any_of(rest.begin(), rest.end(), [](const QString& w) { return things.contains(w); })) continue;
        if (isNum) return std::clamp(n, 1, 20);
        if (auto it = named.find(words[i]); it != named.end()) return it->second;
    }
    return 1;
}

namespace {
struct Spot { double time; QString label; };

Spot spot(const QString& request, const QString& typed, const MagicPointers& ptr, const Project& p) {
    const QString t = u' ' + typed.toLower() + u' ';
    std::vector<Clip> vis, story;
    for (const auto& c : p.clips) if (visual(c)) { vis.push_back(c); if (!p.videoTracks.empty() && c.trackID == p.videoTracks.front().id) story.push_back(c); }
    auto byStart = [](std::vector<Clip> v) { std::stable_sort(v.begin(), v.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; }); return v; };
    const std::vector<Clip> shots = story.empty() ? byStart(vis) : byStart(story);
    double first = INFINITY, last = -INFINITY;
    for (const auto& c : vis) { first = std::min(first, c.start); last = std::max(last, c.end()); }
    if (!std::isfinite(first)) { first = 0; last = 0; }
    const bool before = any(t, {" before ", " in front of ", " ahead of ", " just before "});
    auto around = [&](const std::vector<Clip>& clips, const QString& what) -> std::optional<Spot> {
        std::vector<Clip> seen;
        for (const auto& c : clips) if (c.kind != ClipKind::audio) seen.push_back(c);
        const auto& use = seen.empty() ? clips : seen;
        if (use.empty()) return std::nullopt;
        double a = INFINITY, b = -INFINITY;
        for (const auto& c : use) { a = std::min(a, c.start); b = std::max(b, c.end()); }
        return before ? Spot{a, QStringLiteral("before ") + what} : Spot{b, QStringLiteral("after ") + what};
    };
    auto leadOf = [&](int at) { return typed.left(at).toLower().right(16); };
    auto pickedClips = [&] {
        std::vector<Clip> v;
        for (const auto& c : p.clips) if (std::find(ptr.selected.begin(), ptr.selected.end(), c.id) != ptr.selected.end()) v.push_back(c);
        return v;
    };
    for (const auto& f : MagicMention::find(typed, p.assets, MagicMention::clipLabels(p), MagicMention::markerLabels(p))) {
        switch (f.mention.kind) {
        case MagicMention::Kind::selected: {
            const auto picked = pickedClips();
            const auto nonAudio = std::count_if(picked.begin(), picked.end(), [](const Clip& c) { return c.kind != ClipKind::audio; });
            if (auto s = around(picked, nonAudio > 1 ? QStringLiteral("the selected clips") : QStringLiteral("the selected clip"))) return *s;
            break;
        }
        case MagicMention::Kind::clip:
            if (const Clip* c = p.clip(f.mention.id)) if (auto s = around({*c}, p.name(*c))) return *s;
            break;
        case MagicMention::Kind::marker:
            for (const auto& mk : p.markerList()) if (mk.id == f.mention.id) return Spot{mk.time, QStringLiteral("at the marker")};
            break;
        case MagicMention::Kind::playhead: return Spot{ptr.playhead, QStringLiteral("at the playhead")};
        case MagicMention::Kind::media: {
            const QString lead = leadOf(f.at);
            if (!(lead.contains(QLatin1String("after")) || lead.contains(QLatin1String("before")) || lead.contains(QLatin1String("front of")) || lead.contains(QLatin1String("ahead of")))) continue;
            std::vector<Clip> uses;
            for (const auto& c : vis) if (c.assetID == f.mention.id) uses.push_back(c);
            uses = byStart(uses);
            if (!uses.empty()) {
                const MediaAsset* a = p.asset(f.mention.id);
                if (auto s = around({before ? uses.front() : uses.back()}, a ? a->name : QStringLiteral("it"))) return *s;
            }
            break;
        }
        case MagicMention::Kind::range:
            if (ptr.range) return before ? Spot{ptr.range->start, QStringLiteral("before the marked range")} : Spot{ptr.range->end, QStringLiteral("after the marked range")};
            break;
        }
    }
    if (t.contains(QLatin1String("selected")) && !ptr.selected.empty())
        if (auto s = around(pickedClips(), QStringLiteral("the selected clip"))) return *s;
    static const std::map<QString, int> ordinals = {{"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5}, {"sixth", 6}, {"seventh", 7},
                                                    {"eighth", 8}, {"ninth", 9}, {"tenth", 10}};
    const QStringList words = wordsOf(t);
    for (int i = 0; i + 1 < words.size(); ++i) {
        const QString& w = words[i];
        if (!(words[i + 1].startsWith(QLatin1String("clip")) || words[i + 1].startsWith(QLatin1String("shot")))) continue;
        std::optional<int> n;
        if (auto it = ordinals.find(w); it != ordinals.end()) n = it->second;
        else if (w.endsWith(QLatin1String("st")) || w.endsWith(QLatin1String("nd")) || w.endsWith(QLatin1String("rd")) || w.endsWith(QLatin1String("th"))) {
            QString digits;
            for (QChar c : w) if (c.isDigit()) digits += c;
            bool ok = false;
            const int k = digits.toInt(&ok);
            if (ok) n = k;
        }
        if (w == QLatin1String("last")) n = int(shots.size());
        if (!n || *n < 1 || *n > int(shots.size())) continue;
        const bool pointed = t.contains(QLatin1String(" after ")) || t.contains(QLatin1String(" before ")) || t.contains(QLatin1String(" in front of "));
        if (!pointed) return *n == 1 ? Spot{first, QStringLiteral("at the start")} : Spot{last, QStringLiteral("at the end")};
        if (auto s = around({shots[size_t(*n - 1)]}, w == QLatin1String("last") ? QStringLiteral("the last clip") : QStringLiteral("clip %1").arg(*n))) return *s;
    }
    for (int i = 0; i + 1 < words.size(); ++i) {
        if (words[i] != QLatin1String("clip") && words[i] != QLatin1String("shot")) continue;
        bool ok = false;
        const int k = words[i + 1].toInt(&ok);
        if (!ok || k < 1 || k > int(shots.size()) || !(t.contains(QLatin1String(" after ")) || t.contains(QLatin1String(" before ")))) continue;
        if (auto s = around({shots[size_t(k - 1)]}, QStringLiteral("clip %1").arg(k))) return *s;
    }
    if (t.contains(QLatin1String("playhead")) || t.contains(QLatin1String(" here "))) return Spot{ptr.playhead, QStringLiteral("at the playhead")};
    for (const auto& x : MagicTimes::times(request, p.fps)) {
        if (x.unitOnly && !request.left(x.at).toLower().endsWith(QLatin1String("at "))) continue;
        return Spot{std::min(std::max(first, x.seconds), last), QStringLiteral("at ") + MagicMention::clock(x.seconds)};
    }
    if (any(t, {" beginning", " begin", " start", " front", " opening", " open with", " first "})) return Spot{first, QStringLiteral("at the start")};
    return Spot{last, QStringLiteral("at the end")};
}
} // namespace

LocalOutcome plan(const QString& request, const QString& typed, const MagicPointers& ptr, const Project& p) {
    std::vector<Clip> vis;
    for (const auto& c : p.clips) if (visual(c)) vis.push_back(c);
    if (vis.empty()) return said(QStringLiteral("There's no edit on the timeline to add to."));
    const Spot where = spot(request, typed, ptr, p);
    const double at = p.snapFrame(where.time);
    std::vector<Uuid> named;
    for (const auto& f : MagicMention::find(typed, p.assets)) {
        if (f.mention.kind != MagicMention::Kind::media) continue;
        const QString lead = typed.left(f.at).toLower().right(16);
        if (lead.contains(QLatin1String("after")) || lead.contains(QLatin1String("before")) || lead.contains(QLatin1String("front of")) || lead.contains(QLatin1String("ahead of"))) continue;
        named.push_back(f.mention.id);
    }
    IdSet used;
    for (const auto& c : p.clips) if (c.assetID) used.insert(*c.assetID);
    std::vector<Uuid> pool;
    if (named.empty()) {
        for (const auto& a : p.assets) if ((a.kind == MediaKind::video || a.kind == MediaKind::image) && !used.count(a.id)) pool.push_back(a.id);
    } else {
        for (const auto& id : named) if (const MediaAsset* a = p.asset(id); a && a->kind != MediaKind::audio) pool.push_back(id);
    }
    if (pool.empty())
        return said(named.empty() ? QStringLiteral("Every clip in your library is already in the edit. Import more, or name the one to use with @.")
                                  : QStringLiteral("Those are sound files, not clips to show."));
    const size_t n = named.empty() ? std::min(size_t(count(request)), pool.size()) : pool.size();
    std::vector<double> lengths;
    for (const auto& c : vis) lengths.push_back(c.duration);
    std::sort(lengths.begin(), lengths.end());
    const double typical = std::min(5.0, std::max(2.0, lengths[lengths.size() / 2]));
    const Uuid v1 = p.videoTracks[0].id;
    std::vector<Clip> shots;
    double end = at;
    for (size_t k = 0; k < n; ++k) {
        const MediaAsset* asset = p.asset(pool[k]);
        if (!asset) continue;
        const double d = p.snapFrame(asset->kind == MediaKind::image ? typical : std::min(typical, asset->duration));
        if (!(d > 0)) continue;
        const double source = asset->kind == MediaKind::image ? 0 : std::max(0.0, (asset->duration - d) / 2);
        Clip v(v1, asset->kind == MediaKind::image ? ClipKind::image : ClipKind::video, end, d, source);
        v.assetID = asset->id;
        if (asset->kind == MediaKind::video && asset->hasAudio) {
            const Uuid link = newUuid();
            v.linkID = link;
            Clip a(p.audioTracks[0].id, ClipKind::audio, end, d, source);
            a.assetID = asset->id;
            a.linkID = link;
            shots.push_back(v);
            shots.push_back(a);
        } else {
            shots.push_back(v);
        }
        end += d;
    }
    if (shots.empty()) return said(QStringLiteral("Those clips are too short to use."));
    const double added = end - at;
    std::vector<MagicEdit> edits;
    std::vector<Uuid> crossing;
    for (const auto& c : p.clips) {
        if (!(c.start < at - 1e-6 && c.end() > at + 1e-6) || p.isLocked(c.trackID)) continue;
        bool onV1 = c.trackID == v1;
        if (!onV1) for (const auto& id : p.linked({c.id})) if (const Clip* x = p.clip(id); x && x->trackID == v1) onV1 = true;
        if (onV1) crossing.push_back(c.id);
    }
    std::map<Uuid, Uuid> pieces;
    for (const auto& id : crossing) pieces[id] = newUuid();
    if (!crossing.empty()) edits.push_back(bladeEdit(crossing, at, pieces));
    std::vector<Uuid> later;
    for (const auto& c : p.clips) if (c.start >= at - 1e-6 && !p.isLocked(c.trackID)) later.push_back(c.id);
    for (const auto& [k, v] : pieces) later.push_back(v);
    if (!later.empty()) edits.push_back(moveEdit(later, at, at + added));
    Project after = p;
    for (const auto& e : edits) e.apply(after);
    auto free = [&](const Uuid& track) {
        return std::none_of(after.clips.begin(), after.clips.end(), [&](const Clip& c) { return c.trackID == track && c.start < end - 1e-6 && c.end() > at + 1e-6; });
    };
    if (std::any_of(shots.begin(), shots.end(), [](const Clip& c) { return c.kind == ClipKind::audio; }) && !free(p.audioTracks[0].id)) {
        Uuid track;
        const Track* open = nullptr;
        for (const auto& tr : p.audioTracks) if (!tr.locked && free(tr.id)) { open = &tr; break; }
        if (open) {
            track = open->id;
        } else {
            track = newUuid();
            const Uuid id = track;
            edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Add audio track"), [id](Project& q) {
                Track tr(TrackKind::audio);
                tr.id = id;
                q.audioTracks.push_back(tr);
            }));
        }
        for (auto& c : shots) if (c.kind == ClipKind::audio) c.trackID = track;
    }
    QStringList names;
    for (const auto& c : shots) {
        if (c.kind == ClipKind::audio) continue;
        std::vector<Clip> pair{c};
        for (const auto& a : shots) if (a.kind == ClipKind::audio && a.linkID && a.linkID == c.linkID) pair.push_back(a);
        edits.push_back(MagicEdit::place(pair, c.assetID));
        if (const MediaAsset* a = p.asset(c.assetID)) names << a->name;
    }
    const QString list = names.size() <= 3 ? names.join(QStringLiteral(", ")) : QStringLiteral("%1 clips").arg(names.size());
    return planned(p, edits, QStringLiteral("Added %1 %2, %3 seconds.").arg(list, where.label, QString::number(added, 'f', 1)));
}

} // namespace LocalAdd

// MARK: LocalRamp

namespace LocalRamp {

bool asks(const QString& request) {
    const QString t = request.toLower();
    return t.contains(QLatin1String("ramp")) || any(t, {"slow-mo moment", "slow mo moment", "slowmo moment", "slow motion moment", "slow-motion moment"});
}

bool atCuts(const QString& request) { return any(request.toLower(), {"cut", "transition", "between", "into each", "into the next", "whip"}); }

LocalOutcome plan(const QString& request, const MagicPointers& ptr, const Project& p) {
    const QString t = request.toLower();
    const Track* track = nullptr;
    for (const auto& tr : p.videoTracks)
        if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == tr.id && c.kind == ClipKind::video; })) { track = &tr; break; }
    if (!track) return said(QStringLiteral("There are no video shots on the timeline."));
    std::vector<Clip> shots;
    for (const auto& c : p.clips) if (c.trackID == track->id && c.kind == ClipKind::video && c.playsSource() && !p.isLocked(c.trackID)) shots.push_back(c);
    std::stable_sort(shots.begin(), shots.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    if (shots.empty()) return said(QStringLiteral("The main video track is locked, so nothing was changed."));
    const auto found = MagicMention::find(request, p.assets, MagicMention::clipLabels(p));
    IdSet pointed;
    bool selectedMention = false;
    for (const auto& f : found) {
        if (f.mention.kind == MagicMention::Kind::clip) pointed.insert(f.mention.id);
        if (f.mention.kind == MagicMention::Kind::selected) selectedMention = true;
    }
    if (selectedMention || t.contains(QLatin1String("selected")) || pointed.empty()) pointed.insert(ptr.selected.begin(), ptr.selected.end());
    std::vector<Clip> chosen;
    for (const auto& c : p.clips) if (pointed.count(c.id) && c.kind == ClipKind::video && c.playsSource() && !p.isLocked(c.trackID)) chosen.push_back(c);
    std::stable_sort(chosen.begin(), chosen.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    const auto asked = LocalBrain::speed(request);
    std::vector<MagicEdit> edits;
    if (atCuts(request)) {
        const double fast = asked ? (*asked > 1 ? *asked : 4) : 4;
        const auto& set = chosen.size() >= 2 ? chosen : shots;
        int cuts = 0;
        for (size_t i = 0; i < set.size(); ++i) {
            const Clip& c = set[i];
            const bool before = i > 0 && std::abs(set[i - 1].end() - c.start) < 0.5 / p.fps;
            const bool after = i + 1 < set.size() && std::abs(set[i + 1].start - c.end()) < 0.5 / p.fps;
            if (!(before || after)) continue;
            if (after) ++cuts;
            const double p2 = before ? 0.25 : 0, p3 = after ? 0.75 : 1;
            const IdSet ids = p.unlocked(p.linked({c.id}));
            const Uuid id = c.id;
            edits.push_back(MagicEdit::change(id, QStringLiteral("Speed ramp"), [=](Project& q) {
                const Clip* now = q.clip(id);
                if (!now) return;
                q.setRamp(ids, now->speed, 0.0, p2, p3, 1.0, fast, fast);
            }));
        }
        if (edits.empty()) return said(QStringLiteral("There are no cuts between shots to ramp into."));
        return planned(p, edits, QStringLiteral("Ramped up to %1% into %2 cut%3 and back down out of %4.")
                                     .arg(int(std::round(fast * 100))).arg(cuts).arg(cuts == 1 ? QString() : QStringLiteral("s"), cuts == 1 ? QStringLiteral("it") : QStringLiteral("them")));
    }
    const Clip* atPlayhead = nullptr;
    for (const auto& c : shots) if (ptr.playhead >= c.start && ptr.playhead < c.end()) { atPlayhead = &c; break; }
    std::vector<Clip> targets = chosen;
    if (targets.empty()) {
        if (atPlayhead) targets = {*atPlayhead};
        else targets = {*std::max_element(shots.begin(), shots.end(), [](const Clip& a, const Clip& b) { return a.duration < b.duration; })};
    }
    const double speed = asked ? (*asked == 1 ? 0.3 : *asked) : (t.contains(QLatin1String("fast")) || t.contains(QLatin1String("speed up")) ? 3 : 0.3);
    for (const auto& c : targets) {
        const double L = std::max(1e-6, c.sourceLength());
        double mid = 0.5;
        if (ptr.playhead > c.start && ptr.playhead < c.end()) mid = (c.sourceTime(ptr.playhead) - c.sourceStart) / L;
        mid = std::min(std::max(mid, 0.2), 0.8);
        const IdSet ids = p.unlocked(p.linked({c.id}));
        const double around = c.speed;
        edits.push_back(MagicEdit::change(c.id, speed < 1 ? QStringLiteral("Slow-mo ramp") : QStringLiteral("Speed ramp"), [=](Project& q) {
            q.setRamp(ids, speed, mid - 0.2, mid - 0.1, mid + 0.1, mid + 0.2, around, around);
        }));
    }
    const int n = int(targets.size());
    return planned(p, edits, QStringLiteral("%1 a moment in %2 to %3%, easing in and out of it.")
                                 .arg(speed < 1 ? QStringLiteral("Slowed") : QStringLiteral("Sped up"), n == 1 ? QStringLiteral("the shot") : QStringLiteral("%1 shots").arg(n))
                                 .arg(int(std::round(speed * 100))));
}

} // namespace LocalRamp

// MARK: LocalSpeech

namespace LocalSpeech {

bool asks(const QString& request) {
    return any(request.toLower(), {"dead space", "dead air", "pause", "silence", "silent", " um", "ums", "uhs", "filler", "jump cut", "jumpcut",
                                   "tighten the talk", "tighten the speech", "tighten what"});
}

LocalOutcome plan(const Project& p, const std::optional<IdSet>& only, const TranscriptOf& transcript) {
    constexpr double join = 0.45, keepBefore = 0.12, keepAfter = 0.2, shortest = 0.3;
    const Track* track = nullptr;
    for (const auto& tr : p.videoTracks)
        if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == tr.id && c.kind == ClipKind::video; })) { track = &tr; break; }
    if (!track) return said(QStringLiteral("There are no video shots on the timeline."));
    if (p.isLocked(track->id)) return said(QStringLiteral("The main video track is locked, so nothing was changed."));
    std::vector<Clip> shots;
    for (const auto& c : p.clips) if (c.trackID == track->id && c.kind == ClipKind::video && (!only || only->count(c.id))) shots.push_back(c);
    std::stable_sort(shots.begin(), shots.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    std::vector<std::pair<double, double>> cuts;
    int talking = 0, unheard = 0;
    for (const auto& c : shots) {
        if (!c.assetID) continue;
        auto t = transcript(*c.assetID);
        if (!t) { ++unheard; continue; }
        if (!(std::abs(c.speed - 1) < 1e-3) || c.isReversed()) continue;
        const double s0 = c.sourceStart, s1 = c.sourceStart + c.duration;
        std::vector<Transcript::Span> talk;
        for (const auto& s : t->speech(join)) if (s.end > s0 && s.start < s1) talk.push_back(s);
        if (talk.empty()) continue;
        ++talking;
        std::vector<std::pair<double, double>> keep;
        for (const auto& s : talk) {
            const double a = std::max(s0, s.start - keepBefore), b = std::min(s1, s.end + keepAfter);
            if (!keep.empty() && a <= keep.back().second) keep.back().second = std::max(keep.back().second, b);
            else keep.push_back({a, b});
        }
        double at = s0;
        for (const auto& [a, b] : keep) {
            if (a - at >= shortest) cuts.push_back({c.start + (at - s0), c.start + (a - s0)});
            at = b;
        }
        if (s1 - at >= shortest) cuts.push_back({c.start + (at - s0), c.start + (s1 - s0)});
    }
    if (talking == 0)
        return said(unheard > 0 ? QStringLiteral("Editroom is still listening to your clips. Try again in a moment.")
                                : QStringLiteral("Nobody talks in %1, so there were no pauses to cut.").arg(only ? QStringLiteral("those clips") : QStringLiteral("the edit")));
    if (cuts.empty()) return said(QStringLiteral("The talking is already tight: no pauses long enough to cut."));
    double total = 0;
    for (const auto& c : cuts) total += c.second - c.first;
    auto ordered = cuts;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const Uuid trackID = track->id;
    IdSet soundLinks;
    for (const auto& c : p.clips) if (c.trackID == trackID && c.linkID) soundLinks.insert(*c.linkID);
    std::set<Uuid> tracks{trackID};
    for (const auto& c : p.clips) if (c.trackID != trackID && c.linkID && soundLinks.count(*c.linkID)) tracks.insert(c.trackID);
    std::erase_if(tracks, [&](const Uuid& t) { return p.isLocked(t); });
    const MagicEdit edit = MagicEdit::change(shots.front().id, QStringLiteral("Cut pauses"), [ordered, tracks](Project& q) {
        auto onTracks = [&] { IdSet s; for (const auto& c : q.clips) if (tracks.count(c.trackID)) s.insert(c.id); return s; };
        for (const auto& cut : ordered) {
            q.split(onTracks(), cut.first);
            q.split(onTracks(), cut.second);
            std::erase_if(q.clips, [&](const Clip& c) { return tracks.count(c.trackID) && c.start >= cut.first - 1e-6 && c.end() <= cut.second + 1e-6; });
            const double length = cut.second - cut.first;
            for (auto& c : q.clips) if (tracks.count(c.trackID) && c.start >= cut.second - 1e-6) c.start -= length;
        }
    });
    return planned(p, {edit}, QStringLiteral("Cut %1 pause%2 (%3 seconds) from %4 shot%5 with talking.")
                                  .arg(cuts.size()).arg(cuts.size() == 1 ? QString() : QStringLiteral("s")).arg(QString::number(total, 'f', 1))
                                  .arg(talking).arg(talking == 1 ? QString() : QStringLiteral("s")));
}

} // namespace LocalSpeech

// MARK: LocalCombined

namespace LocalCombined {

std::vector<Part> parts(const QString& request, const Project& p) {
    std::vector<Part> out;
    if (LocalAdd::asks(request, p)) out.push_back(Part::add);
    if (LocalTitle::asks(request, p)) out.push_back(Part::title);
    if (LocalCaptions::asks(request, p)) out.push_back(Part::captions);
    return out;
}

LocalOutcome plan(const std::vector<Part>& parts, const QString& request, const QString& typed, const MagicPointers& ptr, const Project& base,
                  const TranscriptOf& transcript) {
    Project now = base;
    std::vector<MagicEdit> edits;
    QStringList saids;
    for (Part part : parts) {
        LocalOutcome o;
        switch (part) {
        case Part::add: o = LocalAdd::plan(request, typed, ptr, now); break;
        case Part::title: o = LocalTitle::plan(request, ptr.playhead, now); break;
        case Part::captions: o = LocalCaptions::plan(request, ptr, now, transcript); break;
        }
        saids << o.said;
        if (!o.plan) continue;
        for (const auto& task : o.plan->tasks) {
            task.edit.apply(now);
            edits.push_back(task.edit);
        }
    }
    const QString text = saids.join(u' ');
    if (edits.empty()) return said(text);
    return planned(base, edits, text);
}

} // namespace LocalCombined

// MARK: LocalPointed

namespace LocalPointed {

std::optional<LocalOutcome> addShown(const QString& request, const LocalRecipe& recipe, const Project& p, std::optional<double> playhead) {
    const QString text = request.toLower();
    const bool adding = any(text, {"add ", "include", "put ", "insert", "throw in", "stick in", "append", "place "});
    std::vector<Clip> edit;
    for (const auto& c : p.clips) if (visual(c) && !p.isLocked(c.trackID)) edit.push_back(c);
    if (!adding || recipe.wholeLibrary || recipe.shown.empty()) return std::nullopt;
    const Uuid id = recipe.shown.front();
    const MediaAsset* asset = p.asset(id);
    if (!asset || !(asset->kind == MediaKind::video || asset->kind == MediaKind::image)) return std::nullopt;
    if (edit.empty() && !playhead) return std::nullopt;
    auto frames = [&](double t) { return p.fps > 0 ? std::round(t * p.fps) / p.fps : t; };
    if (!playhead && std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.assetID == id; }))
        return said(QStringLiteral("%1 is already in your edit.").arg(asset->name));
    const double wanted = recipe.seconds ? std::max(0.5, *recipe.seconds) : 3;
    const double length = frames(asset->kind == MediaKind::image ? wanted : std::min(wanted, asset->duration));
    double at;
    Uuid track;
    auto lastClip = [&]() -> const Clip* {
        const Clip* l = nullptr;
        for (const auto& c : edit) if (!l || c.end() > l->end()) l = &c;
        return l;
    };
    if (playhead) {
        at = frames(*playhead);
        const Clip* under = nullptr;
        for (const auto& c : edit) if (c.start <= at && c.end() > at) { under = &c; break; }
        if (!under) under = lastClip();
        if (under) track = under->trackID;
        else {
            track = p.videoTracks[0].id;
            for (const auto& t : p.videoTracks) if (!p.isLocked(t.id)) { track = t.id; break; }
        }
    } else {
        const Clip* l = lastClip();
        at = frames(l->end());
        track = l->trackID;
    }
    std::vector<double> times;
    if (auto it = recipe.moments.find(id); it != recipe.moments.end()) times = it->second;
    const double middle = times.empty() ? asset->duration / 2 : times[times.size() / 2];
    Clip v(track, asset->kind == MediaKind::image ? ClipKind::image : ClipKind::video, at, length);
    v.assetID = id;
    v.sourceStart = asset->kind == MediaKind::image ? 0 : frames(std::max(0.0, std::min(middle - length / 2, asset->duration - length)));
    std::optional<Uuid> sound;
    for (const auto& c : edit) {
        if (c.trackID != track || !c.linkID) continue;
        for (const auto& a : p.clips) if (a.linkID == c.linkID && a.kind == ClipKind::audio) { sound = a.trackID; break; }
        break;
    }
    if (!sound) for (const auto& t : p.audioTracks) if (!p.isLocked(t.id)) { sound = t.id; break; }
    std::vector<Clip> group{v};
    if (asset->kind == MediaKind::video && asset->hasAudio && sound) {
        const Uuid link = newUuid();
        group[0].linkID = link;
        Clip a(*sound, ClipKind::audio, at, length);
        a.assetID = id;
        a.sourceStart = v.sourceStart;
        a.linkID = link;
        group.push_back(a);
    }
    std::vector<MagicEdit> edits;
    if (playhead) {
        std::set<Uuid> lanes;
        for (const auto& c : group) lanes.insert(c.trackID);
        IdSet affected;
        for (const auto& c : p.clips) {
            if (!(lanes.count(c.trackID) && !p.isLocked(c.trackID) && c.end() > at + 1e-6)) continue;
            if (c.linkID) { for (const auto& x : p.clips) if (x.linkID == c.linkID) affected.insert(x.id); }
            else affected.insert(c.id);
        }
        std::vector<Uuid> across;
        for (const auto& c : p.clips) if (affected.count(c.id) && c.start < at - 1e-6 && c.end() > at + 1e-6) across.push_back(c.id);
        std::map<Uuid, Uuid> pieces;
        for (const auto& a : across) pieces[a] = newUuid();
        if (!across.empty()) edits.push_back(bladeEdit(across, at, pieces));
        std::vector<Uuid> later;
        for (const auto& c : p.clips) if (affected.count(c.id) && c.start >= at - 1e-6) later.push_back(c.id);
        for (const auto& [k, x] : pieces) later.push_back(x);
        if (!later.empty()) edits.push_back(moveEdit(later, at, at + length));
    }
    edits.push_back(MagicEdit::place(group, id));
    const QString whereSaid = !playhead ? QStringLiteral("at the end") : p.clips.empty() ? QString() : QStringLiteral("at the playhead");
    const QString what = recipe.asked.isEmpty() ? asset->name : QStringLiteral("the %1 from %2").arg(recipe.asked, asset->name);
    return planned(p, edits, QStringLiteral("Added a %1-second clip of ").arg(int(std::round(length))) + what
                                 + (whereSaid.isEmpty() ? QStringLiteral(".") : u' ' + whereSaid + u'.'));
}

std::optional<LocalOutcome> reorder(const QString& request, const Project& p) {
    const QString text = request.toLower();
    const auto labels = MagicMention::clipLabels(p);
    const auto found = MagicMention::find(request, p.assets, labels);
    std::vector<std::pair<int, Uuid>> named;
    for (const auto& f : found)
        if (f.mention.kind == MagicMention::Kind::media || f.mention.kind == MagicMention::Kind::clip) named.push_back({f.at, f.mention.id});
    for (const auto& a : LocalBrain::named(request, p.assets)) {
        if (std::any_of(named.begin(), named.end(), [&](const auto& n) { return n.second == a.id; })) continue;
        const int at = int(request.indexOf(bareName(a.name), 0, Qt::CaseInsensitive));
        if (at < 0 || std::any_of(found.begin(), found.end(), [&](const auto& f) { return at >= f.at && at < f.at + f.length; })) continue;
        named.push_back({at, a.id});
    }
    std::stable_sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Uuid> ids;
    for (const auto& n : named) ids.push_back(n.second);
    if (ids.empty()) return std::nullopt;
    enum class Move { swap, before, after, start, end };
    std::optional<Move> move;
    if (ids.size() >= 2 && any(text, {"swap", "switch", "trade places", "flip"})) move = Move::swap;
    else if (ids.size() >= 2 && any(text, {"in front of", "infront of", "before", "ahead of", "earlier than"})) move = Move::before;
    else if (ids.size() >= 2 && any(text, {"after", "behind", "following", "later than"})) move = Move::after;
    else if (any(text, {"to the start", "to the beginning", "to the front", "at the start", "at the beginning", "open on", "open with", "start with", "start on",
                        "begin with", "first shot", "go first", "come first"})) move = Move::start;
    else if (any(text, {"to the end", "at the end", "end on", "end with", "close on", "close with", "last shot", "go last", "come last"})) move = Move::end;
    if (!move) return std::nullopt;
    const Track* track = nullptr;
    for (const auto& t : p.videoTracks)
        if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == t.id && visual(c); })) { track = &t; break; }
    if (!track) return std::nullopt;
    if (p.isLocked(track->id)) return said(QStringLiteral("The main video track is locked, so nothing was moved."));
    std::vector<Clip> shots;
    for (const auto& c : p.clips) if (c.trackID == track->id && visual(c)) shots.push_back(c);
    std::stable_sort(shots.begin(), shots.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    auto name = [&](const Uuid& id) {
        if (const MediaAsset* a = p.asset(id)) return a->name;
        for (const auto& l : labels) if (l.id == id) return l.name;
        return QStringLiteral("that clip");
    };
    auto shot = [&](const Uuid& id) -> std::optional<Clip> {
        for (const auto& c : shots) if (c.id == id) return c;
        for (const auto& c : shots) if (c.assetID == id) return c;
        return std::nullopt;
    };
    for (size_t k = 0; k < ids.size() && k < 2; ++k)
        if (p.clip(ids[k]) && !shot(ids[k]))
            return said(QStringLiteral("Local can only move shots on the main video track, and %1 isn't one. For that, use Claude or ChatGPT.").arg(name(ids[k])));
    const auto a = shot(ids[0]);
    if (!a) return said(QStringLiteral("%1 isn't in the edit, so there's nothing to move.").arg(name(ids[0])));
    std::vector<Clip> order = shots;
    auto indexOf = [&](const Uuid& id) { return size_t(std::find_if(order.begin(), order.end(), [&](const Clip& c) { return c.id == id; }) - order.begin()); };
    QString s;
    switch (*move) {
    case Move::swap:
    case Move::before:
    case Move::after: {
        const auto b = shot(ids[1]);
        if (!b || b->id == a->id) return said(QStringLiteral("%1 isn't in the edit, so there's nothing to move it next to.").arg(name(ids[1])));
        if (*move == Move::swap) {
            std::swap(order[indexOf(a->id)], order[indexOf(b->id)]);
            s = QStringLiteral("Swapped %1 and %2.").arg(name(ids[0]), name(ids[1]));
        } else {
            order.erase(order.begin() + long(indexOf(a->id)));
            const size_t j = indexOf(b->id);
            order.insert(order.begin() + long(*move == Move::before ? j : j + 1), *a);
            s = QStringLiteral("Put %1 %2 %3.").arg(name(ids[0]), *move == Move::before ? QStringLiteral("in front of") : QStringLiteral("after"), name(ids[1]));
        }
        break;
    }
    case Move::start:
        order.erase(order.begin() + long(indexOf(a->id)));
        order.insert(order.begin(), *a);
        s = QStringLiteral("Moved %1 to the start.").arg(name(ids[0]));
        break;
    case Move::end:
        order.erase(order.begin() + long(indexOf(a->id)));
        order.push_back(*a);
        s = QStringLiteral("Moved %1 to the end.").arg(name(ids[0]));
        break;
    }
    bool same = true;
    for (size_t i = 0; i < order.size(); ++i) if (order[i].id != shots[i].id) same = false;
    if (same) return said(s.chopped(1) + QStringLiteral(" — it was already there, so nothing changed."));
    double t = shots[0].start;
    std::map<Uuid, double> deltas;
    for (const auto& c : order) { deltas[c.id] = t - c.start; t += c.duration; }
    const MagicEdit edit = MagicEdit::change(a->id, QStringLiteral("Move"), [deltas](Project& q) {
        std::map<Uuid, double> links;
        for (const auto& c : q.clips) if (auto it = deltas.find(c.id); it != deltas.end() && c.linkID) links.emplace(*c.linkID, it->second);
        for (auto& c : q.clips) {
            if (auto it = deltas.find(c.id); it != deltas.end()) c.start += it->second;
            else if (c.linkID) if (auto l = links.find(*c.linkID); l != links.end() && !q.isLocked(c.trackID)) c.start += l->second;
        }
    });
    return planned(p, {edit}, s);
}

std::optional<LocalOutcome> plan(const QString& request, const LocalRecipe& recipe, const MagicPointers& ptr, const Project& p) {
    const auto found = MagicMention::find(request, p.assets, MagicMention::clipLabels(p));
    std::set<MagicMention::Kind> kinds;
    std::vector<Uuid> named;
    for (const auto& f : found) {
        kinds.insert(f.mention.kind);
        if (f.mention.kind == MagicMention::Kind::clip) named.push_back(f.mention.id);
    }
    const QString text = request.toLower();
    const bool selection = !named.empty() || kinds.count(MagicMention::Kind::selected)
        || (!ptr.selected.empty() && any(text, {"selected", "selection", "this clip", "this shot", "these clips", "these shots", "that clip", "that shot",
                                                 "those clips", "those shots"}));
    const auto typedSpan = MagicTimes::span(request, p.fps);
    const bool range = typedSpan || kinds.count(MagicMention::Kind::range) || any(text, {"in/out", "in-out", "in and out points", "marked range", "the range"});
    const bool playhead = kinds.count(MagicMention::Kind::playhead) || text.contains(QLatin1String("playhead"));
    if (auto moved = reorder(request, p)) return moved;
    const bool atPlayhead = kinds.count(MagicMention::Kind::playhead) || any(text, {"playhead", "right here", "at this point"});
    const bool empty = std::none_of(p.clips.begin(), p.clips.end(), visual);
    if (auto added = addShown(request, recipe, p, atPlayhead || empty ? std::optional<double>(ptr.playhead) : std::nullopt)) return added;
    if (!(selection || range || playhead)) return std::nullopt;

    const bool remove = any(text, {"remove", "delete", "take out", "take it out", "take them out", "get rid", "cut out", "cut it", "cut them", "drop", "no good",
                                   "not good", "bad", "don't like", "dont like", "hate", "useless", "lose ", "kill"});
    const auto speed = recipe.speed;
    const auto look = recipe.look;
    std::vector<MagicEdit> edits;
    auto lookEdits = [&](const std::vector<Uuid>& ids, const Project& q) {
        std::vector<MagicEdit> out;
        if (!look) return out;
        for (const auto& c : q.clips) {
            if (std::find(ids.begin(), ids.end(), c.id) == ids.end() || !visual(c)) continue;
            if (*look == QLatin1String("none")) {
                if (!c.grade || !c.grade->look) continue;
                const Uuid id = c.id;
                out.push_back(MagicEdit::change(id, QStringLiteral("No look"), [id](Project& x) {
                    if (auto i = x.index(id)) { ColorGrade g = x.clips[*i].grade.value_or(ColorGrade()); g.look.reset(); x.clips[*i].grade = g; }
                }));
            } else if (!c.grade || c.grade->look != *look) {
                out.push_back(lookEdit(c.id, *look));
            }
        }
        return out;
    };
    const QString cant = QStringLiteral("For anything more, use Claude or ChatGPT.");

    if (selection) {
        std::vector<Clip> chosen;
        for (const auto& id : named.empty() ? ptr.selected : named) if (const Clip* c = p.clip(id)) chosen.push_back(*c);
        if (chosen.empty()) return said(QStringLiteral("Nothing was selected. Select clips on the timeline first, then ask again."));
        IdSet chosenIds;
        for (const auto& c : chosen) chosenIds.insert(c.id);
        const IdSet ids = p.unlocked(p.linked(chosenIds));
        if (ids.empty()) return said(QStringLiteral("The selected clips are on locked tracks, so nothing was changed."));
        const int n = int(std::count_if(chosen.begin(), chosen.end(), [&](const Clip& c) { return ids.count(c.id) && c.kind != ClipKind::audio; }));
        QStringList names;
        for (const auto& c : chosen) names << p.name(c);
        const QString what = named.empty() ? plural(std::max(n, 1), QStringLiteral("selected clip"))
                                           : QStringLiteral("clip") + (chosen.size() == 1 ? QStringLiteral(" ") : QStringLiteral("s ")) + names.join(QStringLiteral(", "));
        if (remove && !speed && !look) return planned(p, {removeEdit(ids, true)}, QStringLiteral("Took out the %1 and closed the gap.").arg(what));
        QStringList parts;
        if (speed) {
            const double sp = *speed;
            edits.push_back(MagicEdit::change(chosen.front().id, QStringLiteral("Speed ×%1").arg(g(sp)), [ids, sp](Project& q) { q.setSpeed(ids, sp); }));
            parts << QStringLiteral("set the %1 to %2% speed").arg(what, g(sp * 100));
        }
        const auto looks = lookEdits(std::vector<Uuid>(ids.begin(), ids.end()), p);
        edits.insert(edits.end(), looks.begin(), looks.end());
        if (look && !looks.empty())
            parts << (*look == QLatin1String("none") ? QStringLiteral("took the look off")
                                                    : QStringLiteral("gave %1 the %2 look").arg(speed ? QStringLiteral("them") : QStringLiteral("the ") + what, *look));
        if (edits.empty()) {
            if (look) return said(QStringLiteral("The %1 already %2.").arg(what, *look == QLatin1String("none") ? QStringLiteral("have no look") : QStringLiteral("have the %1 look").arg(*look)));
            return said(QStringLiteral("Local can take out, speed up, slow down or give a look to the selected clips. ") + cant);
        }
        return planned(p, edits, capitalizedFirst(parts.join(QStringLiteral(" and "))) + u'.');
    }

    if (range) {
        std::optional<Range> r;
        if (typedSpan) r = Range{typedSpan->start, std::min(typedSpan->end, p.duration())};
        else r = ptr.range;
        if (!r || !(r->end > r->start))
            return said(typedSpan ? QStringLiteral("That time is past the end of the edit.") : QStringLiteral("There's no in/out range set. Mark one with I and O, then ask again."));
        const Range rr = *r;
        const QString span = QStringLiteral("%1s to %2s").arg(QString::number(rr.start, 'f', 1), QString::number(rr.end, 'f', 1));
        if (any(text, {"only", "keep ", "just "}) && !any(text, {"remove", "delete", "take out", "get rid", "cut out"}))
            return planned(p, {MagicEdit::change(std::nullopt, QStringLiteral("Keep range"), [rr](Project& q) { q = q.cropped(rr); })},
                           QStringLiteral("Kept just %1 and took out the rest.").arg(span));
        if (remove && !speed && !look) {
            return planned(p, {MagicEdit::change(std::nullopt, QStringLiteral("Remove range"), [rr](Project& q) {
                               q.split(q.unlocked(allIds(q)), rr.start);
                               q.split(q.unlocked(allIds(q)), rr.end);
                               std::erase_if(q.clips, [&](const Clip& c) { return c.start >= rr.start - 1e-6 && c.end() <= rr.end + 1e-6 && !q.isLocked(c.trackID); });
                               for (auto& c : q.clips) if (c.start >= rr.end - 1e-6 && !q.isLocked(c.trackID)) c.start -= rr.end - rr.start;
                               q.inPoint.reset();
                               q.outPoint.reset();
                           })},
                           QStringLiteral("Took out %1 and closed the gap.").arg(span));
        }
        if (speed || look) {
            Project q = p;
            q.split(p.unlocked(allIds(p)), rr.start);
            q.split(q.unlocked(allIds(q)), rr.end);
            std::vector<Uuid> inside;
            for (const auto& c : q.clips) if (c.start >= rr.start - 1e-6 && c.end() <= rr.end + 1e-6 && !q.isLocked(c.trackID)) inside.push_back(c.id);
            edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Cut at range"), [rr](Project& x) {
                x.split(x.unlocked(allIds(x)), rr.start);
                x.split(x.unlocked(allIds(x)), rr.end);
            }));
            QStringList parts;
            if (speed) {
                const double sp = *speed;
                edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Speed ×%1").arg(g(sp)), [rr, sp](Project& x) {
                    IdSet ids;
                    for (const auto& c : x.clips) if (c.start >= rr.start - 1e-6 && c.end() <= rr.end + 1e-6 && !x.isLocked(c.trackID)) ids.insert(c.id);
                    x.setSpeed(ids, sp);
                }));
                parts << QStringLiteral("set %1 to %2% speed").arg(span, g(sp * 100));
            }
            const auto looks = lookEdits(inside, q);
            if (look && !looks.empty()) {
                const QString lk = *look;
                edits.push_back(MagicEdit::change(std::nullopt, lk == QLatin1String("none") ? QStringLiteral("No look") : lk, [rr, lk](Project& x) {
                    for (auto& c : x.clips) {
                        if (!(c.start >= rr.start - 1e-6 && c.end() <= rr.end + 1e-6 && !x.isLocked(c.trackID) && visual(c))) continue;
                        ColorGrade gr = c.grade.value_or(ColorGrade());
                        if (lk == QLatin1String("none")) gr.look.reset(); else gr.look = lk;
                        c.grade = gr;
                    }
                }));
                parts << (lk == QLatin1String("none") ? QStringLiteral("took the look off %1").arg(span) : QStringLiteral("gave %1 the %2 look").arg(span, lk));
            }
            if (parts.isEmpty()) return said(QStringLiteral("%1 already looks like that.").arg(span));
            return planned(p, edits, capitalizedFirst(parts.join(QStringLiteral(" and "))) + u'.');
        }
        return said(QStringLiteral("Local can take out, keep only, speed up, slow down or give a look to the in/out range. ") + cant);
    }

    const double t = ptr.playhead;
    const QString at = QString::number(t, 'f', 1) + u's';
    const IdSet all = p.unlocked(allIds(p));
    if (any(text, {"after", "end here", "end at", "end it", "ends at", "ends here", "stop at", "stop here", "finish", "rest of"})) {
        if (std::none_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return all.count(c.id) && c.end() > t + 1e-3; }))
            return said(QStringLiteral("Nothing comes after the playhead."));
        return planned(p, {MagicEdit::change(std::nullopt, QStringLiteral("End here"), [t](Project& q) {
                           q.split(q.unlocked(allIds(q)), t);
                           std::erase_if(q.clips, [&](const Clip& c) { return c.start >= t - 1e-6 && !q.isLocked(c.trackID); });
                       })},
                       QStringLiteral("Ended it at %1, taking out everything after.").arg(at));
    }
    if (any(text, {"before", "start here", "start at", "start it", "starts at", "starts here", "begin", "open at", "open here"})) {
        if (!(t > 1e-3)) return said(QStringLiteral("The playhead is at the start, so there's nothing before it."));
        return planned(p, {MagicEdit::change(std::nullopt, QStringLiteral("Start here"), [t](Project& q) {
                           q.split(q.unlocked(allIds(q)), t);
                           std::erase_if(q.clips, [&](const Clip& c) { return c.end() <= t + 1e-6 && !q.isLocked(c.trackID); });
                           for (auto& c : q.clips) if (!q.isLocked(c.trackID)) c.start = std::max(0.0, c.start - t);
                       })},
                       QStringLiteral("Started it at %1, taking out everything before.").arg(at));
    }
    if (any(text, {"split", "cut", "blade", "slice"})) {
        IdSet under;
        for (const auto& c : p.clips) if (all.count(c.id) && c.start < t - 1e-3 && c.end() > t + 1e-3) under.insert(c.id);
        if (under.empty()) return said(QStringLiteral("There's no clip under the playhead to split."));
        return planned(p, {MagicEdit::change(*under.begin(), QStringLiteral("Split"), [under, t](Project& q) { q.split(under, t); })},
                       QStringLiteral("Split %1 at %2.").arg(plural(int(under.size()), QStringLiteral("clip")), at));
    }
    return said(QStringLiteral("At the playhead, Local can split the clips there, or end or start the video there. ") + cant);
}

} // namespace LocalPointed

} // namespace er
