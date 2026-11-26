#include "Project.h"
#include "Captions.h"
#include "MediaFolders.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <deque>

namespace er {

// MARK: Leaf helpers (Types.h)

QString fadeCurveLabel(FadeCurve c) {
    switch (c) {
    case FadeCurve::linear: return QStringLiteral("Linear");
    case FadeCurve::easeIn: return QStringLiteral("Exponential");
    case FadeCurve::easeOut: return QStringLiteral("Logarithmic");
    case FadeCurve::smooth: return QStringLiteral("S-Curve");
    }
    return {};
}

double applyFadeCurve(FadeCurve c, double x) {
    x = std::min(1.0, std::max(0.0, x));
    switch (c) {
    case FadeCurve::linear: return x;
    case FadeCurve::easeIn: return x * x * x;
    case FadeCurve::easeOut: return 1 - std::pow(1 - x, 3);
    case FadeCurve::smooth: return x * x * (3 - 2 * x);
    }
    return x;
}

QString transitionKindLabel(TransitionKind k) {
    switch (k) {
    case TransitionKind::dissolve: return QStringLiteral("Dissolve");
    case TransitionKind::dipToBlack: return QStringLiteral("Dip to Black");
    case TransitionKind::wipe: return QStringLiteral("Wipe");
    }
    return {};
}

quint32 Marker::hex(Tint t) {
    switch (t) {
    case Tint::white: return 0xEDEDEA;
    case Tint::yellow: return 0xF2B33D;
    case Tint::green: return 0x6CC08B;
    case Tint::blue: return 0x5B9BF0;
    case Tint::purple: return 0xB07CF0;
    case Tint::red: return 0xF0503C;
    }
    return 0;
}

QString MediaFolder::name() const { return QFileInfo(path).fileName(); }

QString Transcript::text() const {
    QStringList parts;
    for (const auto& w : words) parts << w.text;
    return parts.join(u' ');
}

// MARK: Transcript (Transcripts.swift)

QStringList Transcript::tokens(const QString& s) {
    QStringList out;
    QString cur;
    for (QChar c : s.toLower()) {
        if (c.isLetter() || c.isNumber() || c == u'\'') cur += c;
        else if (!cur.isEmpty()) { out << cur; cur.clear(); }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

std::vector<Transcript::Span> Transcript::speech(double gap) const {
    std::vector<Span> out;
    for (const auto& w : words) {
        if (!out.empty() && w.start - out.back().end < gap) out.back().end = std::max(out.back().end, w.end);
        else out.push_back({w.start, w.end});
    }
    return out;
}

std::vector<Transcript::Sentence> Transcript::sentences(double gap) const {
    struct S { double start, end; QStringList words; };
    std::vector<S> out;
    for (const auto& w : words) {
        const bool ends = out.empty() || out.back().words.isEmpty() || out.back().words.last().endsWith(u'.')
                       || out.back().words.last().endsWith(u'?') || out.back().words.last().endsWith(u'!');
        if (!out.empty() && !ends && w.start - out.back().end < gap) {
            out.back().end = w.end;
            out.back().words << w.text;
        } else {
            out.push_back({w.start, w.end, {w.text}});
        }
    }
    std::vector<Sentence> r;
    for (const auto& s : out) r.push_back({s.start, s.end, s.words.join(u' ')});
    return r;
}

bool Transcript::says(const QString& phrase, bool typing) const {
    const QStringList want = tokens(phrase);
    if (want.isEmpty()) return false;
    const QStringList have = tokens(text());
    if (have.size() < want.size()) return false;
    for (qsizetype i = 0; i + want.size() <= have.size(); ++i) {
        bool whole = true;
        for (qsizetype k = 0; k + 1 < want.size() && whole; ++k) whole = have[i + k] == want[k];
        const QString& last = have[i + want.size() - 1];
        if (whole && (typing ? last.startsWith(want.last()) : last == want.last())) return true;
    }
    return false;
}

std::optional<Transcript::Snippet> Transcript::snippet(const QString& phrase, bool typing) const {
    const QStringList want = tokens(phrase);
    if (want.isEmpty() || qsizetype(words.size()) < want.size()) return std::nullopt;
    QStringList have;
    for (const auto& w : words) { const auto t = tokens(w.text); have << (t.isEmpty() ? QString() : t.first()); }
    const qsizetype n = qsizetype(words.size());
    for (qsizetype i = 0; i + want.size() <= n; ++i) {
        bool whole = true;
        for (qsizetype k = 0; k + 1 < want.size() && whole; ++k) whole = have[i + k] == want[k];
        const QString& last = have[i + want.size() - 1];
        if (!(whole && (typing ? last.startsWith(want.last()) : last == want.last()))) continue;
        const qsizetype from = std::max<qsizetype>(0, i - 3), to = std::min(n, i + want.size() + 4);
        auto join = [&](qsizetype a, qsizetype b) { QStringList l; for (qsizetype k = a; k < b; ++k) l << words[size_t(k)].text; return l.join(u' '); };
        return Snippet{(from > 0 ? QStringLiteral("…") : QString()) + join(from, i), join(i, i + want.size()),
                       join(i + want.size(), to) + (to < n ? QStringLiteral("…") : QString())};
    }
    return std::nullopt;
}

std::vector<Transcript::Occurrence> Transcript::occurrences(const QString& phrase) const {
    const QStringList want = tokens(phrase);
    std::vector<Occurrence> out;
    if (want.isEmpty() || qsizetype(words.size()) < want.size()) return out;
    QStringList have;
    for (const auto& w : words) { const auto t = tokens(w.text); have << (t.isEmpty() ? QString() : t.first()); }
    const qsizetype n = qsizetype(words.size());
    for (qsizetype i = 0; i + want.size() <= n; ++i) {
        if (have.mid(i, want.size()) != want) continue;
        const qsizetype from = std::max<qsizetype>(0, i - 4), to = std::min(n, i + want.size() + 5);
        QStringList ctx;
        for (qsizetype k = from; k < to; ++k) ctx << words[size_t(k)].text;
        out.push_back({words[size_t(i)].start, words[size_t(i + want.size() - 1)].end, ctx.join(u' ')});
    }
    return out;
}

Uuid TrackToken::newVideo() { static const Uuid u(QStringLiteral("00000000-0000-0000-0000-0000000000A1")); return u; }
Uuid TrackToken::newAudio() { static const Uuid u(QStringLiteral("00000000-0000-0000-0000-0000000000A2")); return u; }

double TransitionEdge::progress(double t) const {
    return std::clamp((t - start) / std::max(1e-6, end - start), 0.0, 1.0);
}

// MARK: Clip

double Clip::fadeGain(double t) const {
    double g = 1.0;
    if (fadeIn > 0) g = std::min(g, applyFadeCurve(inCurve(), (t - start) / fadeIn));
    if (fadeOut > 0) g = std::min(g, applyFadeCurve(outCurve(), (end() - t) / fadeOut));
    return std::max(0.0, std::min(1.0, g));
}

void Clip::clampFades() {
    fadeIn = std::min(std::max(0.0, fadeIn), duration);
    fadeOut = std::min(std::max(0.0, fadeOut), duration - fadeIn);
}

void Clip::trimHead(double t) {
    const double d = t - start;
    const double newSource = sourceTime(t);
    start = t;
    duration -= d;
    if (playsSource()) sourceStart = std::max(0.0, newSource);
    clampFades();
    // A zoom stays on the picture it was on.
    if (transform && transform->zoom) transform->zoom->shift(d, duration);
}

void Clip::trimTail(double t) {
    duration = t - start;
    clampFades();
    if (transform && transform->zoom) transform->zoom->fit(duration);
}

// MARK: Project queries

double Project::duration() const {
    double d = 0;
    bool any = false;
    for (const auto& c : clips) { d = any ? std::max(d, c.end()) : c.end(); any = true; }
    return any ? d : 0;
}

double Project::snapFrame(double t) const { return std::round(t * fps) / fps; }

const MediaAsset* Project::asset(const std::optional<Uuid>& id) const {
    if (!id) return nullptr;
    for (const auto& a : assets) if (a.id == *id) return &a;
    return nullptr;
}

const Clip* Project::clip(const Uuid& id) const {
    for (const auto& c : clips) if (c.id == id) return &c;
    return nullptr;
}

const Track* Project::track(const Uuid& id) const {
    for (const auto& t : videoTracks) if (t.id == id) return &t;
    for (const auto& t : audioTracks) if (t.id == id) return &t;
    return nullptr;
}

bool Project::isLocked(const Uuid& trackID) const {
    const Track* t = track(trackID);
    return t ? t->locked : false;
}

IdSet Project::unlocked(const IdSet& ids) const {
    std::set<Uuid> locked;
    for (const auto& t : videoTracks) if (t.locked) locked.insert(t.id);
    for (const auto& t : audioTracks) if (t.locked) locked.insert(t.id);
    if (locked.empty()) return ids;
    IdSet out;
    for (const auto& c : clips) if (ids.count(c.id) && !locked.count(c.trackID)) out.insert(c.id);
    return out;
}

bool Project::isAudible(const Track& t) const {
    if (t.muted) return false;
    const bool anySolo = std::any_of(audioTracks.begin(), audioTracks.end(), [](const Track& a) { return a.solo; });
    return !anySolo || t.solo;
}

std::optional<size_t> Project::index(const Uuid& id) const {
    for (size_t i = 0; i < clips.size(); ++i) if (clips[i].id == id) return i;
    return std::nullopt;
}

std::optional<size_t> Project::trackIndex(const Uuid& id, TrackKind kind) const {
    const auto& ts = tracks(kind);
    for (size_t i = 0; i < ts.size(); ++i) if (ts[i].id == id) return i;
    return std::nullopt;
}

QString Project::name(const Clip& c) const {
    switch (c.kind) {
    case ClipKind::text: {
        QString s = c.text.string;
        s.replace(u'\n', u' ');
        return s.isEmpty() ? QStringLiteral("Text") : s;
    }
    case ClipKind::solid: return QStringLiteral("Solid");
    default: {
        const MediaAsset* a = asset(c.assetID);
        return a ? a->name : QStringLiteral("Missing");
    }
    }
}

IdSet Project::linked(const IdSet& ids) const {
    std::set<Uuid> links;
    for (const auto& c : clips) if (ids.count(c.id) && c.linkID) links.insert(*c.linkID);
    if (links.empty()) return ids;
    IdSet out = ids;
    for (const auto& c : clips) if (c.linkID && links.count(*c.linkID)) out.insert(c.id);
    return out;
}

std::optional<double> Project::sourceLimit(const Clip& c) const {
    if (!c.playsSource()) return std::nullopt;
    const MediaAsset* a = asset(c.assetID);
    if (!a) return std::nullopt;
    return a->duration;
}

std::vector<Marker> Project::markerList() const {
    std::vector<Marker> out = markers.value_or(std::vector<Marker>{});
    std::stable_sort(out.begin(), out.end(), [](const Marker& a, const Marker& b) { return a.time < b.time; });
    return out;
}

std::optional<Range> Project::playRange() const {
    if (!inPoint && !outPoint) return std::nullopt;
    const double a = inPoint.value_or(0), b = outPoint ? *outPoint : duration();
    const double lo = std::min(a, b), hi = std::max(a, b);
    if (hi - lo >= 1 / fps) return Range{lo, hi};
    return std::nullopt;
}

Project Project::cropped(Range range) const {
    Project p = *this;
    IdSet all;
    for (const auto& c : p.clips) all.insert(c.id);
    p.split(all, range.start);
    IdSet all2;
    for (const auto& c : p.clips) all2.insert(c.id);
    p.split(all2, range.end);
    std::erase_if(p.clips, [&](const Clip& c) { return c.end() <= range.start + 1e-6 || c.start >= range.end - 1e-6; });
    for (auto& c : p.clips) c.start -= range.start;
    if (p.markers) {
        std::vector<Marker> kept;
        for (Marker m : *p.markers) {
            if (m.time >= range.start && m.time <= range.end) { m.time -= range.start; kept.push_back(m); }
        }
        p.markers = kept;
    }
    p.inPoint.reset();
    p.outPoint.reset();
    return p;
}

std::vector<double> Project::snapTargets(const IdSet& excluding) const {
    std::vector<double> out{0};
    if (markers) for (const auto& m : *markers) out.push_back(m.time);
    if (inPoint) out.push_back(*inPoint);
    if (outPoint) out.push_back(*outPoint);
    for (const auto& c : clips) {
        if (excluding.count(c.id)) continue;
        out.push_back(c.start);
        out.push_back(c.end());
    }
    return out;
}

std::vector<double> Project::editPoints() const {
    std::set<double> s{0};
    for (const auto& c : clips) { s.insert(c.start); s.insert(c.end()); }
    if (markers) for (const auto& m : *markers) s.insert(m.time);
    return {s.begin(), s.end()};
}

std::pair<double, double> Project::trimLimits(const Clip& c, bool left, const IdSet& excluding) const {
    std::vector<const Clip*> others;
    for (const auto& o : clips) if (o.trackID == c.trackID && !excluding.count(o.id) && o.id != c.id) others.push_back(&o);
    const double minDur = std::max(Project::minClip, 1 / fps);
    if (left) {
        double prevEnd = 0;
        bool any = false;
        for (auto* o : others) if (o->end() <= c.start + 1e-6) { prevEnd = any ? std::max(prevEnd, o->end()) : o->end(); any = true; }
        if (!any) prevEnd = 0;
        double lo = prevEnd - c.start;
        if (c.playsSource()) lo = std::max(lo, -c.timelineSpan(0, c.sourceStart));
        const double hi = c.duration - minDur;
        return {std::min(lo, hi), hi};
    }
    double nextStart = INFINITY;
    for (auto* o : others) if (o->start >= c.end() - 1e-6) nextStart = std::min(nextStart, o->start);
    double hi = nextStart - c.end();
    if (auto limit = sourceLimit(c)) hi = std::min(hi, c.timelineSpan(c.sourceStart, *limit) - c.duration);
    const double lo = minDur - c.duration;
    return {lo, std::max(lo, hi)};
}

std::optional<Uuid> Project::freeTrack(TrackKind kind, double start, double dur) const {
    for (const auto& t : tracks(kind)) {
        if (t.locked) continue;
        const bool busy = std::any_of(clips.begin(), clips.end(), [&](const Clip& c) {
            return c.trackID == t.id && c.start < start + dur - 1e-6 && c.end() > start + 1e-6;
        });
        if (!busy) return t.id;
    }
    return std::nullopt;
}

std::optional<std::pair<Uuid, double>> Project::firstFreeVideoSpot(double time, double dur) const {
    std::optional<std::pair<Uuid, double>> best;
    for (const auto& t : videoTracks) {
        if (t.locked) continue;
        std::vector<const Clip*> here;
        for (const auto& c : clips) if (c.trackID == t.id) here.push_back(&c);
        std::stable_sort(here.begin(), here.end(), [](auto* a, auto* b) { return a->start < b->start; });
        std::vector<double> candidates{time};
        for (auto* c : here) if (c->end() > time) candidates.push_back(c->end());
        std::sort(candidates.begin(), candidates.end());
        for (double s : candidates) {
            const bool busy = std::any_of(here.begin(), here.end(), [&](auto* c) { return c->start < s + dur - 1e-6 && c->end() > s + 1e-6; });
            if (!busy) {
                if (!best || s < best->second - 1e-9) best = std::make_pair(t.id, s);
                break;
            }
        }
    }
    return best;
}

// MARK: Mutations

Uuid Project::addTrack(TrackKind kind) {
    Track t(kind);
    if (kind == TrackKind::video) videoTracks.push_back(t); else audioTracks.push_back(t);
    return t.id;
}

void Project::removeTrack(const Uuid& id) {
    auto vi = std::find_if(videoTracks.begin(), videoTracks.end(), [&](const Track& t) { return t.id == id; });
    if (vi != videoTracks.end() && videoTracks.size() > 1) {
        videoTracks.erase(vi);
        std::erase_if(clips, [&](const Clip& c) { return c.trackID == id; });
        return;
    }
    auto ai = std::find_if(audioTracks.begin(), audioTracks.end(), [&](const Track& t) { return t.id == id; });
    if (ai != audioTracks.end() && audioTracks.size() > 1) {
        audioTracks.erase(ai);
        std::erase_if(clips, [&](const Clip& c) { return c.trackID == id; });
    }
}

void Project::updateTrack(const Uuid& id, const std::function<void(Track&)>& body) {
    for (auto& t : videoTracks) if (t.id == id) { body(t); break; }
    for (auto& t : audioTracks) if (t.id == id) { body(t); break; }
}

void Project::materializeTokens() {
    std::optional<Uuid> newV, newA;
    for (auto& c : clips) {
        if (c.trackID == TrackToken::newVideo()) {
            if (!newV) newV = addTrack(TrackKind::video);
            c.trackID = *newV;
        } else if (c.trackID == TrackToken::newAudio()) {
            if (!newA) newA = addTrack(TrackKind::audio);
            c.trackID = *newA;
        }
    }
}

void Project::resolveOverlaps(const IdSet& keeping) {
    const double eps = 1e-6;
    std::vector<Clip> movers;
    for (const auto& c : clips) if (keeping.count(c.id)) movers.push_back(c);
    /// The right-hand pieces of clips split in two, by what they were linked to and where they were cut.
    struct Right { Uuid id, link; double from, to; };
    std::vector<Right> rights;
    for (const auto& m : movers) {
        std::vector<Clip> result;
        result.reserve(clips.size() + 1);
        for (Clip c : clips) {
            if (keeping.count(c.id) || c.trackID != m.trackID || c.end() <= m.start + eps || c.start >= m.end() - eps) {
                result.push_back(c);
                continue;
            }
            const bool head = c.start < m.start - eps;
            const bool tail = c.end() > m.end() + eps;
            if (head && tail) {
                Clip right = c;
                right.id = newUuid();
                right.linkID.reset();
                if (c.linkID) rights.push_back({right.id, *c.linkID, m.start, m.end()});
                right.fadeIn = 0;
                right.trimHead(m.end());
                c.fadeOut = 0;
                c.trimTail(m.start);
                result.push_back(c);
                result.push_back(right);
            } else if (head) {
                c.trimTail(m.start);
                result.push_back(c);
            } else if (tail) {
                c.trimHead(m.end());
                result.push_back(c);
            }
        }
        clips = std::move(result);
    }
    // A linked clip split by something dropped across it and its partner (a video with its sound): the two
    // right-hand pieces are linked to each other, as the left-hand ones still are. A piece whose partner wasn't
    // split (a title dropped over the picture only) stays on its own.
    std::map<QString, std::vector<Uuid>> groups;
    for (const auto& r : rights)
        groups[r.link.toString() + u'|' + QString::number(std::llround(r.from * 1000)) + u'|' + QString::number(std::llround(r.to * 1000))].push_back(r.id);
    for (const auto& [key, pieces] : groups) {
        std::set<TrackKind> kinds;
        for (const auto& id : pieces) if (const Clip* c = clip(id)) kinds.insert(trackKindOf(c->kind));
        if (pieces.size() < 2 || kinds.size() < 2) continue;
        const Uuid link = newUuid();
        for (const auto& id : pieces) if (auto i = index(id)) clips[*i].linkID = link;
    }
}

IdSet Project::split(const IdSet& ids, double t) {
    const double minDur = 0.5 / fps;
    IdSet created;
    std::map<Uuid, Uuid> linkMap;
    for (const auto& id : ids) {
        auto i = index(id);
        if (!i) continue;
        const Clip c = clips[*i];
        if (!(t > c.start + minDur && t < c.end() - minDur)) continue;
        Clip right = c;
        right.id = newUuid();
        right.fadeIn = 0;
        right.trimHead(t);
        if (c.linkID) {
            auto it = linkMap.find(*c.linkID);
            const Uuid mapped = it != linkMap.end() ? it->second : newUuid();
            linkMap[*c.linkID] = mapped;
            right.linkID = mapped;
        }
        clips[*i].fadeOut = 0;
        clips[*i].trimTail(t);
        clips.push_back(right);
        created.insert(right.id);
    }
    return created;
}

void Project::remove(const IdSet& ids, bool ripple) {
    std::vector<Clip> removed;
    for (const auto& c : clips) if (ids.count(c.id)) removed.push_back(c);
    std::stable_sort(removed.begin(), removed.end(), [](const Clip& a, const Clip& b) { return a.start > b.start; });
    std::erase_if(clips, [&](const Clip& c) { return ids.count(c.id) > 0; });
    if (!ripple) return;
    for (const auto& r : removed)
        for (auto& c : clips)
            if (c.trackID == r.trackID && c.start >= r.end() - 1e-6) c.start = std::max(0.0, c.start - r.duration);
}

void Project::rippleRemove(double a, double b, const IdSet& tracksIn) {
    IdSet trk;
    for (const auto& t : tracksIn) if (!isLocked(t)) trk.insert(t);
    const double gapLen = b - a;
    if (!(gapLen > 1e-9) || trk.empty()) return;
    auto on = [&](const Clip& c) { return trk.count(c.trackID) > 0; };
    IdSet atA, atB;
    for (const auto& c : clips) if (on(c) && c.start < a && c.end() > a) atA.insert(c.id);
    split(atA, a);
    for (const auto& c : clips) if (on(c) && c.start < b && c.end() > b) atB.insert(c.id);
    split(atB, b);
    const double eps = 0.5 / fps;
    std::erase_if(clips, [&](const Clip& c) { return on(c) && c.start >= a - eps && c.end() <= b + eps; });
    for (auto& c : clips) if (on(c) && c.start >= b - eps) c.start = std::max(0.0, c.start - gapLen);
}

void Project::setSpeed(const IdSet& ids, double speed, bool sticky) {
    speed = std::min(std::max(speed, 0.05), 100.0);
    retime(ids, sticky, [speed](Clip& c) { c.speed = speed; });
}

void Project::setRamp(const IdSet& ids, double speed, double p1, double p2, double p3, double p4,
                      double start, double end, bool sticky) {
    auto clamp = [](double v) { return std::min(std::max(v, 0.05), 100.0); };
    retime(ids, sticky, [&](Clip& c) {
        const double s0 = c.sourceStart, s1 = c.sourceStart + c.sourceLength();
        c.speed = clamp(speed);
        c.ramp = SpeedRamp::drawn(s0, s1, p1, p2, p3, p4, clamp(start), clamp(end), c.speed);
    });
}

void Project::retime(const IdSet& ids, bool sticky, const std::function<void(Clip&)>& change) {
    if (sticky) {
        // Work out the changed clips as usual, then let whatever touched them follow.
        Project plain = *this;
        plain.retime(ids, false, change);
        std::map<Uuid, Clip> edited;
        for (const auto& c : plain.clips) if (ids.count(c.id)) edited[c.id] = c;
        const auto carriedClips = carried(edited);
        for (auto& c : clips) {
            if (auto e = edited.find(c.id); e != edited.end()) c = e->second;
            else if (auto k = carriedClips.find(c.id); k != carriedClips.end()) c = k->second;
        }
        return;
    }
    std::vector<Track> all = videoTracks;
    all.insert(all.end(), audioTracks.begin(), audioTracks.end());
    auto snap = [this](double t) { return snapFrame(t); };
    for (const auto& track : all) {
        std::vector<size_t> order;
        for (size_t i = 0; i < clips.size(); ++i) if (clips[i].trackID == track.id) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return clips[a].start < clips[b].start; });
        if (!std::any_of(order.begin(), order.end(), [&](size_t i) { return ids.count(clips[i].id) && clips[i].playsSource(); })) continue;
        double shift = 0.0;
        double prevEnd = 0.0;
        for (size_t i : order) {
            Clip c = clips[i];
            if (ids.count(c.id) && c.playsSource()) {
                const double before = c.duration;
                c.retime(snap, 1 / fps, change);
                c.start += shift;
                shift += c.duration - before;
            } else if (ids.count(c.id)) {
                c.start += shift;
            }
            c.start = std::max(c.start, prevEnd);
            prevEnd = c.end();
            clips[i] = c;
        }
    }
}

std::map<Uuid, Clip> Project::carried(const std::map<Uuid, Clip>& edited) const {
    const double touch = 0.5 / fps;
    std::map<Uuid, Clip> out;
    struct Q { Uuid track; double oldEnd, newEnd; };
    std::deque<Q> queue;
    for (const auto& [id, e] : edited)
        if (const Clip* o = clip(id)) queue.push_back({o->trackID, o->end(), e.end()});
    while (!queue.empty()) {
        const Q q = queue.front();
        queue.pop_front();
        const double shift = q.newEnd - q.oldEnd;
        if (!(std::abs(shift) > 1e-9)) continue;
        // Only the next clip along can be touching or run into; ones further on are reached through it.
        if (isLocked(q.track)) continue;
        std::vector<const Clip*> after;
        for (const auto& c : clips)
            if (c.trackID == q.track && !edited.count(c.id) && c.start >= q.oldEnd - touch) after.push_back(&c);
        if (after.empty()) continue;
        double nearest = after[0]->start;
        for (auto* c : after) nearest = std::min(nearest, c->start);
        for (auto* c : after) {
            if (!(c->start <= nearest + 1e-9) || out.count(c->id)) continue;
            const bool touching = std::abs(c->start - q.oldEnd) <= touch;
            const bool runInto = shift > 0 && c->start < q.newEnd - 1e-9;
            if (!(touching || runInto)) continue;
            const double s = q.newEnd - c->start;
            for (const auto& id : linked({c->id})) {
                if (edited.count(id) || out.count(id)) continue;
                const Clip* mp = clip(id);
                if (!mp) continue;
                Clip m = *mp;
                const double oldEnd = m.end();
                m.start += s;
                out[id] = m;
                queue.push_back({m.trackID, oldEnd, m.end()});
            }
        }
    }
    return out;
}

bool Project::fits(const std::map<Uuid, Clip>& changed) const {
    for (const auto& [id, c] : changed) if (c.start < -1e-9) return false;
    std::map<Uuid, std::vector<Clip>> groups;
    for (const auto& c : clips) {
        auto it = changed.find(c.id);
        const Clip& x = it != changed.end() ? it->second : c;
        groups[x.trackID].push_back(x);
    }
    for (auto& [track, group] : groups) {
        std::stable_sort(group.begin(), group.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        for (size_t i = 0; i + 1 < group.size(); ++i) {
            const Clip& a = group[i];
            const Clip& b = group[i + 1];
            if (!changed.count(a.id) && !changed.count(b.id)) continue;
            if (a.end() > b.start + 1e-6) return false;
        }
    }
    return true;
}

// MARK: Transitions

const Clip* Project::previousTouching(const Clip& c) const {
    const double tol = 0.5 / fps;
    for (const auto& o : clips)
        if (o.id != c.id && o.trackID == c.trackID && o.kind != ClipKind::audio && std::abs(o.end() - c.start) <= tol) return &o;
    return nullptr;
}

const Clip* Project::nextTouching(const Clip& c) const {
    const double tol = 0.5 / fps;
    for (const auto& o : clips)
        if (o.id != c.id && o.trackID == c.trackID && o.kind != ClipKind::audio && std::abs(o.start - c.end()) <= tol) return &o;
    return nullptr;
}

double Project::headRoom(const Clip& c) const {
    return c.playsSource() ? c.timelineSpan(0, c.sourceStart) : INFINITY;
}

double Project::tailRoom(const Clip& c) const {
    auto limit = sourceLimit(c);
    if (!c.playsSource() || !limit) return INFINITY;
    return std::max(0.0, c.timelineSpan(c.sourceStart + c.sourceLength(), *limit));
}

std::optional<TransitionSpan> Project::transitionSpan(const Clip& a, const Clip& b) const {
    if (!b.transitionIn || b.kind == ClipKind::audio || a.kind == ClipKind::audio) return std::nullopt;
    const Transition& t = *b.transitionIn;
    const double cut = b.start;
    const double frame = 1 / fps;
    if (t.kind == TransitionKind::dipToBlack) {
        const double d = std::min({t.duration, 2 * a.duration, 2 * b.duration});
        if (d >= frame) return TransitionSpan{cut - d / 2, cut + d / 2, false};
        return std::nullopt;
    }
    const double want = std::min({t.duration, a.duration, b.duration});
    const double head = headRoom(b), tail = tailRoom(a);
    if (head >= want) return TransitionSpan{cut - want, cut, true};
    if (tail >= want) return TransitionSpan{cut, cut + want, false};
    const double d = std::max(head, tail);
    if (!(d >= frame)) return std::nullopt;
    return head >= tail ? TransitionSpan{cut - d, cut, true} : TransitionSpan{cut, cut + d, false};
}

TransitionPlan Project::transitionPlan(const Clip& c) const {
    TransitionPlan plan;
    if (c.kind == ClipKind::audio) return plan;
    if (const Clip* a = previousTouching(c)) {
        auto span = transitionSpan(*a, c);
        if (span && c.transitionIn) {
            const auto kind = c.transitionIn->kind;
            if (kind == TransitionKind::dipToBlack) {
                plan.reveal = TransitionEdge{c.start, span->end, kind};
            } else {
                plan.reveal = TransitionEdge{span->start, span->end, kind};
                if (span->fromHead) plan.leadIn = c.start - span->start;
            }
        }
    }
    if (const Clip* b = nextTouching(c)) {
        auto span = transitionSpan(c, *b);
        if (span && b->transitionIn) {
            const auto kind = b->transitionIn->kind;
            if (kind == TransitionKind::dipToBlack) plan.conceal = TransitionEdge{span->start, c.end(), kind};
            else if (!span->fromHead) plan.tailOut = span->end - c.end();
        }
    }
    return plan;
}

// MARK: Gaps

std::optional<Range> Project::gap(const Uuid& trk, double t) const {
    std::vector<const Clip*> here;
    for (const auto& c : clips) if (c.trackID == trk) here.push_back(&c);
    for (auto* c : here) if (t >= c->start && t < c->end()) return std::nullopt;
    double before = 0;
    bool anyBefore = false;
    for (auto* c : here) if (c->end() <= t + 1e-9) { before = anyBefore ? std::max(before, c->end()) : c->end(); anyBefore = true; }
    std::optional<double> after;
    for (auto* c : here) if (c->start >= t - 1e-9) after = after ? std::min(*after, c->start) : c->start;
    if (!after || !(*after - before > 1e-6)) return std::nullopt;
    return Range{before, *after};
}

double Project::pullLeft(const Uuid& trk, double time, double amount) {
    if (isLocked(trk) || !(amount > 1e-9)) return 0;
    IdSet seed;
    for (const auto& c : clips) if (c.trackID == trk && c.start >= time - 1e-6) seed.insert(c.id);
    const IdSet movers = unlocked(linked(seed));
    if (movers.empty()) return 0;
    auto moved = [&](double d) {
        std::map<Uuid, Clip> out;
        for (const auto& c : clips) if (movers.count(c.id)) { Clip m = c; m.start -= d; out[c.id] = m; }
        return out;
    };
    double d = amount;
    if (!fits(moved(d))) {
        double ok = 0.0, bad = d;
        for (int i = 0; i < 30; ++i) { const double mid = (ok + bad) / 2; if (fits(moved(mid))) ok = mid; else bad = mid; }
        d = snapFrame(ok) <= ok + 1e-9 ? snapFrame(ok) : ok;
        if (!fits(moved(d))) d = ok;
    }
    if (!(d > 1e-9)) return 0;
    for (const auto& [id, c] : moved(d)) if (auto i = index(id)) clips[*i] = c;
    return d;
}

void Project::closeAllGaps(const Uuid& trk) {
    for (int n = 0; n < 500; ++n) {
        std::vector<Clip> here;
        for (const auto& c : clips) if (c.trackID == trk) here.push_back(c);
        std::stable_sort(here.begin(), here.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        std::optional<size_t> found;
        for (size_t i = 0; i + 1 < here.size(); ++i) if (here[i + 1].start - here[i].end() > 1e-6) { found = i; break; }
        if (!found) return;
        const Clip next = here[*found + 1];
        const double shift = next.start - here[*found].end();
        if (pullLeft(trk, next.start, shift) < 1e-9) return;
    }
}

void Project::unlink(const IdSet& ids) {
    const IdSet all = linked(ids);
    for (auto& c : clips) if (all.count(c.id)) c.linkID.reset();
}

void Project::fitClips(const IdSet& toMedia) {
    IdSet gone;
    for (auto& c0 : clips) {
        const Clip c = c0;
        auto limit = sourceLimit(c);
        if (!c.assetID || !toMedia.count(*c.assetID) || !limit || !(*limit > 0) ||
            !(c.sourceStart + c.sourceLength() > *limit + 1e-4)) continue;
        if (c.sourceStart >= *limit - 1e-3) { gone.insert(c.id); continue; }
        // Where on the timeline the clip reaches the file's end.
        double lo = c.start, hi = c.end();
        for (int i = 0; i < 40; ++i) {
            const double m = (lo + hi) / 2;
            if (c.sourceTime(m) < *limit) lo = m; else hi = m;
        }
        c0.trimTail(lo);
    }
    std::erase_if(clips, [&](const Clip& c) { return gone.count(c.id) > 0; });
}

void Project::sanitize() {
    std::set<Uuid> trackIDs;
    for (const auto& t : videoTracks) trackIDs.insert(t.id);
    for (const auto& t : audioTracks) trackIDs.insert(t.id);
    std::erase_if(clips, [&](const Clip& c) { return !trackIDs.count(c.trackID) || c.duration <= 1e-6; });
    for (auto& c : clips) {
        c.start = std::max(0.0, c.start);
        c.clampFades();
        const double length = c.duration;
        if (c.transform && c.transform->zoom) c.transform->zoom->fit(length);
    }
    // The caption track goes with the last captioned clip (deleted, or its captions taken off): it could
    // never show anything again. Undo brings both back.
    if (captions && !std::any_of(clips.begin(), clips.end(), [](const Clip& c) { return c.captioned == true; })) {
        captions.reset();
        captionFixes.reset();
    }
}

// MARK: Paths

/// URL(fileURLWithPath:).standardizedFileURL.pathComponents: "/" then each part, with "." and ".." resolved.
static QStringList pathComponents(const QString& path) {
    QString p = QDir::cleanPath(QDir::isAbsolutePath(path) ? path : QDir::current().absoluteFilePath(path));
    QStringList parts = p.split(u'/', Qt::SkipEmptyParts);
    parts.prepend(QStringLiteral("/"));
    return parts;
}

static QString relative(const QStringList& base, const QStringList& file) {
    qsizetype shared = 0;
    while (shared < base.size() && shared < file.size() && base[shared] == file[shared]) ++shared;
    QStringList parts;
    for (qsizetype i = 0; i < base.size() - shared; ++i) parts << QStringLiteral("..");
    for (qsizetype i = shared; i < file.size(); ++i) parts << file[i];
    return parts.join(u'/');
}

void Project::stampRelativePaths(const QString& projectDir) {
    const QStringList base = pathComponents(projectDir);
    for (auto& a : assets) a.relativePath = relative(base, pathComponents(a.path));
    if (folders) for (auto& f : *folders) f.relativePath = relative(base, pathComponents(f.path));
}

int Project::resolveMovedMedia(const QString& projectDir) {
    int found = 0;
    auto standardized = [&](const QString& rel) { return QDir::cleanPath(projectDir + u'/' + rel); };
    // A folder brought in whole, moved along with the project: everything in it moves with it.
    if (folders) {
        const auto copy = *folders;
        for (const auto& f : copy) {
            if (QFileInfo::exists(f.path) || !f.relativePath) continue;
            const QString candidate = standardized(*f.relativePath);
            if (MediaFolders::isFolder(candidate)) movePaths(f.path, candidate);
        }
    }
    for (auto& a : assets) {
        if (QFileInfo::exists(a.path) || !a.relativePath) continue;
        const QString candidate = standardized(*a.relativePath);
        if (QFileInfo::exists(candidate)) { a.path = candidate; ++found; }
    }
    return found;
}

const MediaFolder* Project::linkedFolder(const QString& path) const {
    const MediaFolder* best = nullptr;
    if (!folders) return nullptr;
    for (const auto& f : *folders)
        if (MediaFolders::contains(f.path, path) && (!best || best->path.size() < f.path.size())) best = &f;
    return best;
}

void Project::movePaths(const QString& old, const QString& now) {
    if (old == now) return;
    auto moved = [&](const QString& p) -> std::optional<QString> {
        if (MediaFolders::contains(old, p)) return now + p.mid(old.size());
        return std::nullopt;
    };
    for (auto& a : assets) {
        if (auto p = moved(a.path)) a.path = *p;
        if (a.playablePath) if (auto p = moved(*a.playablePath)) a.playablePath = *p;
    }
    if (folders) for (auto& f : *folders) if (auto p = moved(f.path)) f.path = *p;
    if (hiddenMedia) for (auto& h : *hiddenMedia) if (auto p = moved(h)) h = *p;
}

bool Project::isHidden(const QString& path) const {
    if (!hiddenMedia) return false;
    for (const auto& h : *hiddenMedia) if (MediaFolders::contains(h, path)) return true;
    return false;
}

void Project::pruneFolders(const std::set<Uuid>& keeping) {
    if (!folders) return;
    std::vector<MediaFolder> kept;
    for (const auto& folder : *folders) {
        const bool used = keeping.count(folder.id) ||
            std::any_of(assets.begin(), assets.end(), [&](const MediaAsset& a) { return MediaFolders::contains(folder.path, a.path); });
        if (used) kept.push_back(folder);
    }
    if (kept.empty()) folders.reset(); else folders = kept;
    // Hidden files outside every remaining folder don't matter any more.
    if (hiddenMedia) {
        std::erase_if(*hiddenMedia, [&](const QString& h) {
            return !std::any_of(kept.begin(), kept.end(), [&](const MediaFolder& f) { return MediaFolders::contains(f.path, h); });
        });
        if (hiddenMedia->empty()) hiddenMedia.reset();
    }
}

// MARK: dB

double dbToLinear(double db) { return db <= -59.5 ? 0 : std::pow(10.0, db / 20); }

QString formatDB(double db) {
    if (db <= -59.5) return QStringLiteral("−∞ dB");
    const double v = std::round(db * 10) / 10;
    const QString sign = v > 0 ? QStringLiteral("+") : v < 0 ? QStringLiteral("−") : QString();
    return sign + QString::asprintf("%.1f dB", std::abs(v));
}

} // namespace er
