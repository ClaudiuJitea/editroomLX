#include "LocalImprover.h"
#include "analysis/LocalBrain.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace er::LocalImprover {

MagicPlan sequential(const Project& base, const std::vector<MagicEdit>& edits) {
    MagicPlan plan{base, {}};
    for (size_t i = 0; i < edits.size(); ++i) plan.tasks.push_back({edits[i], i > 0 ? std::vector<int>{int(i) - 1} : std::vector<int>{}, 0});
    return plan;
}

namespace {

MagicEdit kindEdit(MagicEdit::Kind k) { MagicEdit e; e.kind = k; return e; }
MagicEdit removeEdit(const IdSet& ids) { MagicEdit e = kindEdit(MagicEdit::Kind::remove); e.ids.assign(ids.begin(), ids.end()); return e; }
MagicEdit removeEdit(const Uuid& id) { return removeEdit(IdSet{id}); }
MagicEdit trimEdit(bool head, const IdSet& ids, double from, double to) {
    MagicEdit e = kindEdit(head ? MagicEdit::Kind::trimHead : MagicEdit::Kind::trimTail);
    e.ids.assign(ids.begin(), ids.end());
    e.from = from;
    e.to = to;
    return e;
}
MagicEdit moveEdit(const IdSet& ids, double from, double to) {
    MagicEdit e = kindEdit(MagicEdit::Kind::move);
    e.ids.assign(ids.begin(), ids.end());
    e.from = from;
    e.to = to;
    return e;
}
MagicEdit fadeEdit(bool in, const Uuid& id, double to) { MagicEdit e = kindEdit(in ? MagicEdit::Kind::fadeIn : MagicEdit::Kind::fadeOut); e.clip = id; e.to = to; return e; }
MagicEdit gainEdit(const Uuid& id, double from, double to) { MagicEdit e = kindEdit(MagicEdit::Kind::gain); e.clip = id; e.from = from; e.to = to; return e; }
MagicEdit lookEdit(const Uuid& id, const QString& name) { MagicEdit e = kindEdit(MagicEdit::Kind::look); e.clip = id; e.text = name; return e; }
MagicEdit exposureEdit(const Uuid& id, double to) { MagicEdit e = kindEdit(MagicEdit::Kind::grade); e.clip = id; e.key = MagicGradeKey::exposure; e.to = to; return e; }

bool isShot(const Clip& c) { return (c.kind == ClipKind::video || c.kind == ClipKind::image) && c.assetID; }

double sourceRoom(const Clip& c, const Project& p) {
    auto limit = p.sourceLimit(c);
    if (!limit) return c.duration;
    return std::max(c.duration, (*limit - c.sourceStart) / std::max(0.01, c.speed));
}

int beatIndex(int offset, double time, const MusicAnalysis& m, const Clip& song) {
    const double st = song.sourceTime(time);
    if (m.beats.empty()) return offset;
    int best = 0;
    for (int i = 1; i < int(m.beats.size()); ++i) if (std::abs(m.beats[size_t(i)] - st) < std::abs(m.beats[size_t(best)] - st)) best = i;
    return best;
}

enum class MomentKind { cut, motion };
struct Moment { double time; MomentKind kind; double score; };

std::vector<Moment> strongMoments(const std::vector<Clip>& keep, const std::vector<double>& cuts, const std::map<Uuid, ClipAnalysis>& analyses) {
    auto samples = [&](const Clip& c) {
        std::vector<ClipAnalysis::Sample> out;
        if (!c.assetID) return out;
        auto it = analyses.find(*c.assetID);
        if (it == analyses.end()) return out;
        const double lo = c.sourceStart, hi = c.sourceStart + c.sourceLength();
        for (const auto& s : it->second.samples) if (c.kind == ClipKind::image || (s.time >= lo && s.time <= hi)) out.push_back(s);
        return out;
    };
    auto edge = [&](const Clip& c, bool end) -> std::optional<std::pair<double, double>> {
        const auto s = samples(c);
        if (s.empty()) return std::nullopt;
        std::vector<ClipAnalysis::Sample> part;
        if (end) part.assign(s.end() - std::min<ptrdiff_t>(2, s.size()), s.end());
        else part.assign(s.begin(), s.begin() + std::min<ptrdiff_t>(2, s.size()));
        double l = 0, col = 0;
        for (const auto& x : part) { l += x.luma; col += x.color; }
        return std::make_pair(l / double(part.size()), col / double(part.size()));
    };
    std::vector<Moment> out;
    for (size_t k = 0; k + 1 < keep.size(); ++k) {
        auto a = edge(keep[k], true), b = edge(keep[k + 1], false);
        if (!a || !b) continue;
        const double score = std::abs(a->first - b->first) * 3 + std::abs(a->second - b->second) * 1.5 + (keep[k].assetID != keep[k + 1].assetID ? 0.1 : 0);
        if (score >= 0.5) out.push_back({cuts[k + 1], MomentKind::cut, score});
    }
    std::vector<double> all;
    for (const auto& c : keep) for (const auto& s : samples(c)) all.push_back(s.motion);
    std::sort(all.begin(), all.end());
    if (all.size() >= 8) {
        const double median = all[all.size() / 2], p95 = all[size_t(double(all.size() - 1) * 0.95)];
        const double bar = std::max({p95, median * 3, 0.02});
        for (size_t k = 0; k < keep.size(); ++k) {
            const Clip& c = keep[k];
            for (const auto& s : samples(c)) {
                if (s.motion < bar) continue;
                const double t = cuts[k] + (s.time - c.sourceStart) / std::max(0.01, c.speed);
                if (std::none_of(out.begin(), out.end(), [&](const Moment& m) { return std::abs(m.time - t) < 1.5; }))
                    out.push_back({t, MomentKind::motion, 0.5 + s.motion / std::max(bar, 1e-6) * 0.1});
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Moment& a, const Moment& b) { return a.score > b.score; });
    return out;
}

QString plural(int n, const QString& one, const QString& many) { return n == 1 ? one : many; }
QString fmt1(double v) { return QString::number(v, 'f', 1); }

} // namespace

std::optional<Outcome> plan(const Project& p, const std::map<Uuid, ClipAnalysis>& analyses, const std::optional<MusicAnalysis>& music,
                            const LocalRecipe& recipe, const std::optional<MediaAsset>& newSong) {
    auto levelEdits = [&](const Project& q) {
        std::vector<MagicEdit> out;
        for (const auto& c : q.clips) {
            auto it = recipe.levelGains.find(c.id);
            if (it != recipe.levelGains.end() && std::abs(it->second - c.gainDB) > 0.05) out.push_back(gainEdit(c.id, c.gainDB, it->second));
        }
        return out;
    };
    auto levelPart = [&](const std::vector<MagicEdit>& edits) {
        const int n = int(std::count_if(edits.begin(), edits.end(), [](const MagicEdit& e) { return e.kind == MagicEdit::Kind::gain; }));
        const int s = int(recipe.levelSilent.size());
        const QString silent = s == 0 ? QString()
            : QStringLiteral(" (%1 %2 no sound, so %3 left as %4)").arg(recipe.levelSilent.join(QStringLiteral(", ")), s == 1 ? QStringLiteral("has") : QStringLiteral("have"),
                                                                       s == 1 ? QStringLiteral("it was") : QStringLiteral("they were"), s == 1 ? QStringLiteral("it is") : QStringLiteral("they are"));
        return QStringLiteral("set %1 audio clip%2 to the same level").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")) + silent;
    };
    // Only the audio levels (and maybe a look): just that.
    if (recipe.onlyLevels()) {
        auto edits = levelEdits(p);
        const bool levelled = !edits.empty();
        if (recipe.look && *recipe.look != QLatin1String("none"))
            for (const auto& c : p.clips)
                if ((c.kind == ClipKind::video || c.kind == ClipKind::image) && (!c.grade || c.grade->look != *recipe.look)) edits.push_back(lookEdit(c.id, *recipe.look));
        if (recipe.levelGains.empty()) return Outcome{MagicPlan{p, {}}, {}, {}, true, QStringLiteral("There's no audio on the timeline to level.")};
        if (edits.empty()) return Outcome{MagicPlan{p, {}}, {}, {}, true, QStringLiteral("Your audio clips are already at the same level.")};
        QStringList parts;
        if (levelled) parts << levelPart(edits);
        if (recipe.look && *recipe.look != QLatin1String("none")) parts << QStringLiteral("one look (%1)").arg(*recipe.look);
        return Outcome{sequential(p, edits), parts.join(QStringLiteral(", ")), {}, true, std::nullopt};
    }
    // The main picture track: the lowest video track with shots on it.
    const Track* track = nullptr;
    for (const auto& t : p.videoTracks)
        if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == t.id && isShot(c); })) { track = &t; break; }
    if (!track || p.isLocked(track->id)) return std::nullopt;
    const Uuid trackID = track->id;
    std::vector<Clip> shots;
    for (const auto& c : p.clips) if (c.trackID == trackID && isShot(c)) shots.push_back(c);
    std::stable_sort(shots.begin(), shots.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    if (shots.empty()) return std::nullopt;

    std::vector<ClipAnalysis::Sample> all;
    for (const auto& c : shots) if (auto it = analyses.find(*c.assetID); it != analyses.end()) all.insert(all.end(), it->second.samples.begin(), it->second.samples.end());
    const auto score = all.empty() ? std::function<double(const ClipAnalysis::Sample&)>([](const ClipAnalysis::Sample&) { return 0.5; }) : AutoPlanner::scorer(all);
    const double fps = p.fps;
    auto frames = [fps](double t) { return std::round(t * fps) / fps; };
    auto analysisOf = [&](const Clip& c) -> const ClipAnalysis* {
        if (!c.assetID) return nullptr;
        auto it = analyses.find(*c.assetID);
        return it == analyses.end() ? nullptr : &it->second;
    };
    auto shotScore = [&](const Clip& c) {
        const ClipAnalysis* a = analysisOf(c);
        if (!a) return 0.5;
        const double lo = c.sourceStart, hi = c.sourceStart + c.sourceLength();
        double sum = 0;
        int n = 0;
        for (const auto& s : a->samples) if (c.kind == ClipKind::image || (s.time >= lo && s.time <= hi)) { sum += score(s); ++n; }
        return n == 0 ? 0.5 : sum / n;
    };
    /// The best window of `length` (timeline seconds) inside a shot's current part of its file.
    auto best = [&](const Clip& c, double length) -> std::pair<double, double> {
        const ClipAnalysis* a = analysisOf(c);
        if (c.kind != ClipKind::video || !a || a->samples.empty()) return {c.sourceStart, shotScore(c)};
        const double lo = c.sourceStart, hi = c.sourceStart + c.sourceLength(), span = length * c.speed;
        double bestStart = c.sourceStart, bestScore = -INFINITY;
        for (double s = lo; s + span <= hi + 1e-6; s += 0.1) {
            std::vector<double> inside;
            for (const auto& x : a->samples) if (x.time >= s && x.time <= s + span) inside.push_back(score(x));
            if (inside.empty()) continue;
            const double v = std::accumulate(inside.begin(), inside.end(), 0.0) / double(inside.size()) * 0.75 + *std::min_element(inside.begin(), inside.end()) * 0.25;
            if (v > bestScore) { bestScore = v; bestStart = s; }
        }
        return {bestStart, std::isfinite(bestScore) ? bestScore : shotScore(c)};
    };

    // A speed for every shot: that (with any look asked for too), and nothing else.
    if (recipe.speed) {
        const double speed = *recipe.speed;
        std::vector<MagicEdit> edits;
        IdSet shotIDs;
        for (const auto& c : shots) shotIDs.insert(c.id);
        const IdSet ids = p.unlocked(p.linked(shotIDs));
        if (std::any_of(shots.begin(), shots.end(), [&](const Clip& c) { return c.kind == ClipKind::video && std::abs(c.speed - speed) > 1e-3; }))
            edits.push_back(MagicEdit::change(shots.front().id, QStringLiteral("Speed ×%1").arg(speed), [ids, speed](Project& q) { q.setSpeed(ids, speed); }));
        if (recipe.look && *recipe.look != QLatin1String("none"))
            for (const auto& c : shots) if (!c.grade || c.grade->look != *recipe.look) edits.push_back(lookEdit(c.id, *recipe.look));
        Project q = p;
        for (const auto& e : edits) e.apply(q);
        const auto levels = levelEdits(q);
        edits.insert(edits.end(), levels.begin(), levels.end());
        if (edits.empty()) return std::nullopt;
        double beforeEnd = 0, afterEnd = 0;
        for (const auto& c : shots) beforeEnd = std::max(beforeEnd, c.end());
        for (const auto& c : q.clips) if (c.trackID == trackID) afterEnd = std::max(afterEnd, c.end());
        QStringList parts{QStringLiteral("set every clip to %1% speed (%2s to %3s)").arg(speed * 100).arg(fmt1(beforeEnd - shots.front().start), fmt1(afterEnd - shots.front().start))};
        if (recipe.look) parts << QStringLiteral("one look (%1)").arg(*recipe.look);
        if (!levels.empty()) parts << levelPart(levels);
        return Outcome{sequential(p, edits), parts.join(QStringLiteral(", ")), {}, true, std::nullopt};
    }

    // 0. Media they named: only those, or not those.
    const bool focused = recipe.isFocused();
    std::vector<Clip> keep = shots, dropped;
    auto assetOf = [](const Clip& c) { return c.assetID.value_or(Uuid()); };
    if (!recipe.keepOnly.empty()) {
        for (const auto& c : keep) if (!recipe.keepOnly.count(assetOf(c))) dropped.push_back(c);
        std::erase_if(keep, [&](const Clip& c) { return !recipe.keepOnly.count(assetOf(c)); });
    }
    if (!recipe.takeOut.empty()) {
        for (const auto& c : keep) if (recipe.takeOut.count(assetOf(c))) dropped.push_back(c);
        std::erase_if(keep, [&](const Clip& c) { return recipe.takeOut.count(assetOf(c)) > 0; });
    }
    const int named = int(dropped.size());
    if (keep.empty()) return std::nullopt;

    // 1. Repeats: the same file, mostly the same part. Keep the better-scoring one.
    for (size_t i = focused ? keep.size() : 0; i < keep.size(); ++i) {
        size_t j = i + 1;
        while (j < keep.size()) {
            const Clip a = keep[i], b = keep[j];
            if (a.assetID && a.assetID == b.assetID && a.kind == b.kind) {
                const double alo = a.sourceStart, ahi = a.sourceStart + a.sourceLength(), blo = b.sourceStart, bhi = b.sourceStart + b.sourceLength();
                const double overlap = std::max(0.0, std::min(ahi, bhi) - std::max(alo, blo));
                const double shorter = std::max(0.01, std::min(ahi - alo, bhi - blo));
                if (a.kind == ClipKind::image || overlap / shorter > 0.5) {
                    if (shotScore(b) > shotScore(a)) { dropped.push_back(a); keep[i] = b; keep.erase(keep.begin() + long(j)); continue; }
                    dropped.push_back(b);
                    keep.erase(keep.begin() + long(j));
                    continue;
                }
            }
            ++j;
        }
    }
    const int repeats = int(dropped.size()) - named;

    // 2. The weakest shots, when there are enough to spare: at most a quarter, never below four.
    if (keep.size() > 4 && !focused) {
        std::vector<double> scores;
        for (const auto& c : keep) scores.push_back(shotScore(c));
        const double floor = AutoPlanner::percentile(scores, 0.15) - 0.05;
        std::vector<size_t> weak;
        for (size_t i = 0; i < keep.size(); ++i) if (scores[i] < floor) weak.push_back(i);
        std::stable_sort(weak.begin(), weak.end(), [&](size_t a, size_t b) { return scores[a] < scores[b]; });
        const size_t canDrop = std::min({weak.size(), keep.size() / 4, keep.size() - 4});
        IdSet gone;
        for (size_t i = 0; i < canDrop; ++i) gone.insert(keep[weak[i]].id);
        for (const auto& c : keep) if (gone.count(c.id)) dropped.push_back(c);
        std::erase_if(keep, [&](const Clip& c) { return gone.count(c.id) > 0; });
    }
    std::stable_sort(keep.begin(), keep.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });

    // 3. How long each shot plays, never longer than now.
    auto paced = [&](const Clip& c) {
        switch (recipe.pace) {
        case LocalRecipe::Pace::calm: return c.duration > 6 ? std::max(4.0, c.duration * 0.8) : c.duration;
        case LocalRecipe::Pace::fast: return c.duration > 2 ? std::max(1.2, std::min(2.5, c.duration * 0.5)) : c.duration;
        case LocalRecipe::Pace::unchanged:
            if (focused) return c.duration;
            return c.duration > 3.5 ? std::min(c.duration, std::max(2.5, std::min(4.5, c.duration * 0.6))) : c.duration;
        }
        return c.duration;
    };
    QStringList notes;
    std::map<Uuid, double> planned;
    for (const auto& c : keep) planned.emplace(c.id, paced(c));
    bool growing = false;
    auto fileLength = [&](const Clip& c) {
        if (c.kind == ClipKind::image) return 30.0;
        const MediaAsset* a = p.asset(c.assetID);
        return std::max(c.duration, (a ? a->duration : c.duration) / std::max(0.01, c.speed));
    };
    auto sumPaced = [&] { double s = 0; for (const auto& c : keep) s += paced(c); return s; };
    if (recipe.seconds) {
        const double target = *recipe.seconds;
        double pacedTotal = sumPaced();
        while (pacedTotal * 0.6 > target && keep.size() > 2) {
            size_t weakest = 0;
            for (size_t i = 1; i < keep.size(); ++i) if (shotScore(keep[i]) < shotScore(keep[weakest])) weakest = i;
            dropped.push_back(keep[weakest]);
            keep.erase(keep.begin() + long(weakest));
            pacedTotal = sumPaced();
        }
        double nowTotal = 0;
        for (const auto& c : keep) nowTotal += c.duration;
        if (target <= pacedTotal) {
            for (const auto& c : keep) planned[c.id] = std::max(0.8, paced(c) * target / std::max(0.01, pacedTotal));
        } else if (target <= nowTotal) {
            const double f = (target - pacedTotal) / std::max(0.01, nowTotal - pacedTotal);
            for (const auto& c : keep) planned[c.id] = paced(c) + (c.duration - paced(c)) * f;
        } else {
            growing = true;
            std::map<Uuid, double> lengths;
            for (const auto& c : keep) lengths.emplace(c.id, c.duration);
            double need = target - nowTotal;
            for (int round = 0; round < 6 && need > 0.05; ++round) {
                std::vector<Clip> open;
                for (const auto& c : keep) if (fileLength(c) - lengths[c.id] > 0.05) open.push_back(c);
                if (open.empty()) break;
                const double share = need / double(open.size());
                for (const auto& c : open) {
                    const double add = std::min(share, fileLength(c) - lengths[c.id]);
                    lengths[c.id] += add;
                    need -= add;
                }
            }
            planned = lengths;
            if (need > 0.5)
                notes << QStringLiteral("it can't be %1 seconds long: all the footage in your shots only adds up to about %2").arg(int(target)).arg(int(std::round(target - need)));
        }
    }
    auto wanted = [&](const Clip& c) { auto it = planned.find(c.id); return it != planned.end() ? it->second : paced(c); };
    const int weakOut = int(dropped.size()) - repeats - named;

    // The song.
    std::optional<Clip> songOnTimeline;
    for (const auto& c : p.clips) {
        if (c.kind != ClipKind::audio || c.linkID) continue;
        const MediaAsset* a = p.asset(c.assetID);
        if (!(a && a->kind == MediaKind::audio && a->duration >= 8)) continue;
        if (!songOnTimeline || c.duration > songOnTimeline->duration) songOnTimeline = c;
    }
    const double firstStart = shots.front().start;
    std::optional<Clip> song = recipe.music == LocalRecipe::Music::off ? std::nullopt : songOnTimeline;
    std::optional<Clip> placing;
    if (recipe.music == LocalRecipe::Music::add && !songOnTimeline && newSong && music) {
        double bar = 0;
        for (size_t i = 0; i < music->beats.size(); ++i) if (music->isBarStart(int(i))) { bar = music->beats[i]; break; }
        Clip c(newUuid(), ClipKind::audio, firstStart, std::max(1.0, newSong->duration - bar));
        c.assetID = newSong->id;
        c.sourceStart = bar;
        placing = c;
        song = c;
    } else if (recipe.music == LocalRecipe::Music::add && !songOnTimeline) {
        notes << QStringLiteral("there's no music in your Media to add");
    }
    std::vector<double> cuts{firstStart};
    std::vector<double> lengths;
    const bool snap = !focused || recipe.seconds || recipe.pace != LocalRecipe::Pace::unchanged || placing;
    if (snap && song && music && music->beats.size() > 8) {
        const MusicAnalysis& m = *music;
        std::vector<double> beats;
        for (double b : m.beats) {
            const double t = song->start + (b - song->sourceStart) / std::max(0.01, song->speed);
            if (t > firstStart + 0.2) beats.push_back(t);
        }
        const double minLen = std::max(0.8, 2 * m.period());
        for (size_t k = 0; k < keep.size(); ++k) {
            const Clip& c = keep[k];
            const bool last = k == keep.size() - 1;
            const double from = cuts.back();
            const double target = from + wanted(c);
            const double room = growing ? fileLength(c) : last ? (c.kind == ClipKind::image ? 3600 : sourceRoom(c, p)) : c.duration;
            std::vector<std::pair<int, double>> options;
            for (size_t i = 0; i < beats.size(); ++i) if (beats[i] >= from + minLen - 1e-6 && beats[i] <= from + room + 1e-6) options.push_back({int(i), beats[i]});
            auto nearest = [&](const std::vector<std::pair<int, double>>& o) -> std::optional<double> {
                if (o.empty()) return std::nullopt;
                auto it = std::min_element(o.begin(), o.end(), [&](const auto& a, const auto& b) { return std::abs(a.second - target) < std::abs(b.second - target); });
                return it->second;
            };
            double pick;
            if (last) {
                std::vector<std::pair<int, double>> bars;
                for (const auto& o : options) if (m.isBarStart(beatIndex(o.first, o.second, m, *song))) bars.push_back(o);
                auto b = nearest(bars);
                if (!b) b = nearest(options);
                pick = b ? *b : from + std::min(wanted(c), room);
            } else {
                auto b = nearest(options);
                pick = b ? *b : from + wanted(c);
            }
            lengths.push_back(frames(pick - from));
            cuts.push_back(from + lengths.back());
        }
    } else {
        for (const auto& c : keep) {
            lengths.push_back(frames(wanted(c)));
            cuts.push_back(cuts.back() + lengths.back());
        }
    }
    const double newEnd = cuts.back();
    double oldEnd = newEnd;
    if (!shots.empty()) { oldEnd = 0; for (const auto& c : shots) oldEnd = std::max(oldEnd, c.end()); }

    // 4. The edits.
    std::vector<MagicEdit> edits;
    std::optional<std::pair<int, int>> size;
    QString frameName;
    switch (recipe.frame) {
    case LocalRecipe::Frame::vertical: size = std::make_pair(1080, 1920); frameName = QStringLiteral("vertical"); break;
    case LocalRecipe::Frame::horizontal: size = std::make_pair(1920, 1080); frameName = QStringLiteral("horizontal"); break;
    case LocalRecipe::Frame::square: size = std::make_pair(1080, 1080); frameName = QStringLiteral("square"); break;
    case LocalRecipe::Frame::unchanged: break;
    }
    if (size && (p.width != size->first || p.height != size->second)) {
        const auto sz = *size;
        edits.push_back(MagicEdit::change(std::nullopt, frameName.left(1).toUpper() + frameName.mid(1), [sz](Project& q) {
            q.width = sz.first;
            q.height = sz.second;
            q.settingsLocked = true;
        }));
    }
    for (const auto& c : dropped) edits.push_back(removeEdit(p.unlocked(p.linked({c.id}))));
    int trimmed = 0;
    if (growing) {
        for (const auto& c : keep) edits.push_back(removeEdit(p.unlocked(p.linked({c.id}))));
        for (size_t k = 0; k < keep.size(); ++k) {
            const Clip& c = keep[k];
            const double length = lengths[k];
            double from = c.sourceStart;
            const ClipAnalysis* a = analysisOf(c);
            const MediaAsset* asset = p.asset(c.assetID);
            if (c.kind == ClipKind::video && a && asset) {
                const double span = length * c.speed;
                double bestScore = -INFINITY;
                for (double s0 = 0; s0 + span <= asset->duration + 1e-6; s0 += 0.1) {
                    std::vector<double> inside;
                    for (const auto& x : a->samples) if (x.time >= s0 && x.time <= s0 + span) inside.push_back(score(x));
                    if (inside.empty()) continue;
                    const double v = std::accumulate(inside.begin(), inside.end(), 0.0) / double(inside.size()) * 0.75
                                   + *std::min_element(inside.begin(), inside.end()) * 0.25 - std::abs(s0 - c.sourceStart) * 0.01;
                    if (v > bestScore) { bestScore = v; from = s0; }
                }
                if (!std::isfinite(bestScore)) from = std::min(c.sourceStart, std::max(0.0, asset->duration - span));
            }
            std::vector<Clip> group;
            for (const auto& g : p.clips) {
                if (!(g.id == c.id || (c.linkID && g.linkID == c.linkID))) continue;
                Clip n = g;
                n.start = frames(cuts[k]);
                n.duration = frames(length);
                if (n.kind != ClipKind::image) n.sourceStart = from;
                n.clampFades();
                group.push_back(n);
            }
            edits.push_back(MagicEdit::place(group, c.assetID));
            ++trimmed;
        }
    }
    if (!growing) {
        for (size_t k = 0; k < keep.size(); ++k) {
            const Clip& c = keep[k];
            const IdSet ids = p.unlocked(p.linked({c.id}));
            const double length = lengths[k];
            double head = c.start;
            if (c.kind == ClipKind::video && length < c.duration - 0.5 / fps) {
                const auto w = best(c, length);
                head = frames(c.start + (w.first - c.sourceStart) / std::max(0.01, c.speed));
                if (std::abs(head - c.start) > 0.5 / fps) edits.push_back(trimEdit(true, ids, c.start, head));
            }
            const double tail = frames(head + length);
            if (std::abs(tail - c.end()) > 0.5 / fps) {
                edits.push_back(trimEdit(false, ids, c.end(), tail));
                ++trimmed;
            }
            const double target = frames(cuts[k]);
            if (std::abs(target - head) > 0.5 / fps) edits.push_back(moveEdit(ids, head, target));
        }
    }

    // 5. Titles and overlays above.
    std::map<Uuid, int> order;
    for (size_t i = 0; i < p.videoTracks.size(); ++i) order.emplace(p.videoTracks[i].id, int(i));
    auto orderOf = [&](const Uuid& t) { auto it = order.find(t); return it == order.end() ? 0 : it->second; };
    auto isVideoTrack = [&](const Uuid& t) { return order.count(t) > 0; };
    std::vector<Clip> titles;
    for (const auto& c : p.clips) if (c.kind == ClipKind::text) titles.push_back(c);
    std::stable_sort(titles.begin(), titles.end(), [&](const Clip& a, const Clip& b) { return orderOf(a.trackID) < orderOf(b.trackID); });
    IdSet doubled;
    if (recipe.clearRest) {
        for (const auto& c : p.clips)
            if (c.trackID != trackID && isVideoTrack(c.trackID) && !p.isLocked(c.trackID) && !recipe.keepOnly.count(assetOf(c))) {
                doubled.insert(c.id);
                edits.push_back(removeEdit(c.id));
            }
    }
    if (!focused) {
        for (size_t k = 0; k < titles.size(); ++k) {
            const Clip& t = titles[k];
            if (doubled.count(t.id)) continue;
            for (size_t l = k + 1; l < titles.size(); ++l) {
                const Clip& u = titles[l];
                if (doubled.count(u.id) || u.text.string != t.text.string || !(u.start < t.end() && u.end() > t.start) || p.isLocked(u.trackID)) continue;
                doubled.insert(u.id);
                edits.push_back(removeEdit(u.id));
            }
        }
    }
    for (const auto& c : p.clips) {
        if (c.trackID == trackID || !isVideoTrack(c.trackID) || p.isLocked(c.trackID) || doubled.count(c.id)) continue;
        if (c.start >= newEnd - 0.2) edits.push_back(removeEdit(c.id));
        else if (c.end() > newEnd) edits.push_back(trimEdit(false, {c.id}, c.end(), newEnd));
    }

    // 6. Sound.
    IdSet keptIDs;
    for (const auto& c : keep) keptIDs.insert(c.id);
    std::vector<Clip> keptAudio;
    for (const auto& a : p.clips)
        if (a.kind == ClipKind::audio && a.linkID && std::any_of(keep.begin(), keep.end(), [&](const Clip& k) { return k.linkID == a.linkID; })) keptAudio.push_back(a);
    const bool musicGoes = recipe.music == LocalRecipe::Music::off
        || (recipe.clearRest && recipe.music != LocalRecipe::Music::add && !recipe.keepOnly.count(songOnTimeline ? assetOf(*songOnTimeline) : Uuid()));
    if (musicGoes && songOnTimeline && !p.isLocked(songOnTimeline->trackID)) edits.push_back(removeEdit(songOnTimeline->id));
    if (placing && newSong) {
        Clip c = *placing;
        const Track* free = nullptr;
        for (const auto& t : p.audioTracks) {
            if (p.isLocked(t.id)) continue;
            if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& x) { return x.trackID == t.id && x.start < newEnd && x.end() > firstStart; })) continue;
            free = &t;
            break;
        }
        if (free) {
            c.trackID = free->id;
        } else {
            const Uuid id = newUuid();
            edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Add audio track"), [id](Project& q) {
                Track t(TrackKind::audio);
                t.id = id;
                q.audioTracks.push_back(t);
            }));
            c.trackID = id;
        }
        c.duration = frames(newEnd - firstStart);
        edits.push_back(MagicEdit::place({c}, newSong->id));
        edits.push_back(fadeEdit(false, c.id, std::min(2.0, c.duration * 0.15)));
        for (const auto& a : keptAudio) if (a.gainDB > -10) edits.push_back(gainEdit(a.id, a.gainDB, -16));
    } else if (song && !musicGoes && !p.isLocked(song->trackID)) {
        if (song->end() > newEnd + 1e-3) edits.push_back(trimEdit(false, {song->id}, song->end(), newEnd));
        if (song->fadeOut < 1.5) edits.push_back(fadeEdit(false, song->id, std::min(2.0, std::max(0.5, (std::min(song->end(), newEnd) - song->start) * 0.15))));
        for (const auto& a : keptAudio) if (a.gainDB > -10) edits.push_back(gainEdit(a.id, a.gainDB, -16));
    } else if (!focused && !keptAudio.empty()) {
        const Clip* first = &keptAudio.front();
        const Clip* last = &keptAudio.front();
        for (const auto& a : keptAudio) { if (a.start < first->start) first = &a; if (a.start > last->start) last = &a; }
        if (first->fadeIn < 0.2) edits.push_back(fadeEdit(true, first->id, 0.3));
        if (last->fadeOut < 0.3) edits.push_back(fadeEdit(false, last->id, 0.6));
    }

    // 7. One look, and exposure evened out.
    std::optional<QString> common = recipe.look;
    if (!common && !focused) {
        std::map<QString, int> counts;
        for (const auto& c : keep) if (c.grade && c.grade->look) ++counts[*c.grade->look];
        int most = 0;
        for (const auto& [l, n] : counts) if (n > most) { most = n; common = l; }
    }
    int graded = 0;
    for (const auto& c : keep) {
        if (!keptIDs.count(c.id) || c.kind == ClipKind::audio) continue;
        if (common == QStringLiteral("none")) {
            if (c.grade && c.grade->look) {
                const Uuid id = c.id;
                edits.push_back(MagicEdit::change(id, QStringLiteral("No look"), [id](Project& q) {
                    if (auto i = q.index(id)) {
                        ColorGrade g = q.clips[*i].grade.value_or(ColorGrade());
                        g.look.reset();
                        q.clips[*i].grade = g.isNeutral() ? std::nullopt : std::optional<ColorGrade>(g);
                    }
                }));
                ++graded;
            }
        } else if (common && (!c.grade || c.grade->look != *common)) {
            edits.push_back(lookEdit(c.id, *common));
            ++graded;
        }
        if (!focused && (c.grade ? c.grade->exposure : 0.0) == 0) {
            if (const ClipAnalysis* a = analysisOf(c)) {
                const double lo = c.sourceStart, hi = c.sourceStart + c.sourceLength();
                std::vector<double> lumas;
                for (const auto& s : a->samples) if (c.kind == ClipKind::image || (s.time >= lo && s.time <= hi)) lumas.push_back(s.luma);
                if (!lumas.empty()) {
                    const double mean = std::accumulate(lumas.begin(), lumas.end(), 0.0) / double(lumas.size());
                    const double exposure = std::round(std::clamp(std::log2(0.45 / std::max(0.05, mean)) * 0.5, -0.5, 0.5) * 10) / 10;
                    if (std::abs(exposure) >= 0.2) edits.push_back(exposureEdit(c.id, exposure));
                }
            }
        }
    }

    bool judgedNone = false;
    // 8. Sound effects they asked for, on the cuts.
    int effectsPlaced = 0;
    if (recipe.soundEffects) {
        const auto effects = LocalBrain::soundEffects(p);
        if (effects.empty()) {
            notes << QStringLiteral("there are no sound effects in your Media (sound files under 10 seconds)");
        } else {
            auto namedLike = [](const MediaAsset& a, std::initializer_list<const char*> words) {
                for (const char* w : words) if (a.name.toLower().contains(QLatin1String(w))) return true;
                return false;
            };
            std::vector<MediaAsset> swooshes, hits, others;
            for (const auto& e : effects) {
                const bool sw = namedLike(e, {"whoosh", "swoosh", "swish", "woosh", "transition", "riser", "sweep"});
                const bool hi = namedLike(e, {"hit", "impact", "boom", "thud", "punch", "slam", "bass drop", "drop"});
                if (sw) swooshes.push_back(e);
                if (hi) hits.push_back(e);
                if (!sw && !hi) others.push_back(e);
            }
            auto isSwoosh = [&](const MediaAsset& e) { return std::any_of(swooshes.begin(), swooshes.end(), [&](const MediaAsset& s) { return s.id == e.id; }); };
            struct Spot { double time; MediaAsset effect; bool lead; };
            std::vector<Spot> spots;
            const std::vector<MediaAsset>& pool = swooshes.empty() ? (others.empty() ? hits : others) : swooshes;
            if (recipe.askedJudgement) {
                const auto strong = strongMoments(keep, cuts, analyses);
                for (size_t k = 0; k < strong.size() && k < 8; ++k) {
                    const auto& m = strong[k];
                    if (m.kind == MomentKind::motion && !hits.empty()) spots.push_back({m.time, hits.front(), false});
                    else { const auto& e = pool[k % pool.size()]; spots.push_back({m.time, e, isSwoosh(e)}); }
                }
                std::stable_sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.time < b.time; });
                if (spots.empty()) {
                    judgedNone = true;
                    notes << QStringLiteral("Local could add your sound effects, but it doesn't see a strong moment for them: the cuts don't change the picture much, "
                                            "and nothing in the shots moves suddenly. Say where you'd like them (for example \"a whoosh on every cut\" or \"an impact at "
                                            "the start\"), or ask Claude or ChatGPT, which can watch the footage");
                }
            } else {
                if (!hits.empty()) spots.push_back({firstStart, hits.front(), false});
                std::vector<double> cutTimes(cuts.begin() + 1, cuts.end() - (cuts.size() > 1 ? 1 : 0));
                for (size_t k = 0; k < cutTimes.size() && spots.size() < 8; ++k) {
                    const auto& e = pool[k % pool.size()];
                    spots.push_back({cutTimes[k], e, isSwoosh(e)});
                }
                if (cutTimes.empty() && spots.empty() && !pool.empty()) spots.push_back({firstStart, pool.front(), false});
            }
            if (!spots.empty()) {
                std::vector<std::pair<double, double>> spans;
                for (const auto& s : spots) {
                    const double start = std::max(firstStart, s.lead ? s.time - std::min(0.5, s.effect.duration * 0.6) : s.time - 0.03);
                    spans.push_back({start, std::min(newEnd, start + s.effect.duration)});
                }
                double lo = INFINITY, hi = -INFINITY;
                for (const auto& sp : spans) { lo = std::min(lo, sp.first); hi = std::max(hi, sp.second); }
                Uuid effectTrack;
                const Track* free = nullptr;
                for (const auto& t : p.audioTracks) {
                    if (p.isLocked(t.id) || (songOnTimeline && t.id == songOnTimeline->trackID) || (placing && t.id == placing->trackID)) continue;
                    if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& x) { return x.trackID == t.id && x.start < hi && x.end() > lo; })) continue;
                    free = &t;
                    break;
                }
                if (free) {
                    effectTrack = free->id;
                } else {
                    const Uuid id = newUuid();
                    edits.push_back(MagicEdit::change(std::nullopt, QStringLiteral("Add audio track"), [id](Project& q) {
                        Track t(TrackKind::audio);
                        t.id = id;
                        q.audioTracks.push_back(t);
                    }));
                    effectTrack = id;
                }
                double lastEnd = -INFINITY;
                IdSet effectIDs;
                for (const auto& e : effects) effectIDs.insert(e.id);
                for (size_t k = 0; k < spots.size(); ++k) {
                    double start = spans[k].first;
                    const double end = spans[k].second;
                    if (std::any_of(p.clips.begin(), p.clips.end(), [&](const Clip& x) { return effectIDs.count(assetOf(x)) && x.start < end && x.end() > start; })) continue;
                    start = std::max(start, lastEnd);
                    if (!(end - start > 0.1)) continue;
                    Clip c(effectTrack, ClipKind::audio, frames(start), frames(end - start));
                    c.assetID = spots[k].effect.id;
                    c.gainDB = -6;
                    c.fadeOut = std::min(0.15, c.duration / 3);
                    edits.push_back(MagicEdit::place({c}, spots[k].effect.id));
                    lastEnd = end;
                    ++effectsPlaced;
                }
            }
        }
    }

    // The audio levels, once everything else is in place.
    Project final = p;
    for (const auto& e : edits) e.apply(final);
    const auto levels = levelEdits(final);
    edits.insert(edits.end(), levels.begin(), levels.end());

    if (effectsPlaced == 0 && recipe.soundEffects && !judgedNone && notes.isEmpty() && recipe.isFocused())
        notes << QStringLiteral("every cut already has a sound effect, so there was nowhere new to put one");
    if (edits.empty() && (judgedNone || !notes.isEmpty())) {
        QStringList up;
        for (const QString& n : notes) up << n.left(1).toUpper() + n.mid(1);
        const QString text = up.join(QStringLiteral(". "));
        return Outcome{MagicPlan{p, {}}, {}, notes, true, text.isEmpty() ? QStringLiteral("Your edit already looks good; nothing to change.") : text + u'.'};
    }
    if (edits.empty()) return std::nullopt;
    QStringList parts;
    if (!(focused && !recipe.seconds)) parts << QStringLiteral("%1s to %2s").arg(fmt1(oldEnd - firstStart), fmt1(newEnd - firstStart));
    if (effectsPlaced > 0) parts << QStringLiteral("added %1 sound effect%2 on the cuts").arg(effectsPlaced).arg(effectsPlaced == 1 ? QString() : QStringLiteral("s"));
    if (!recipe.keepOnly.empty()) parts << QStringLiteral("kept only %1 (%2 shot%3 out)").arg(recipe.mediaNames.join(QStringLiteral(", "))).arg(named).arg(named == 1 ? QString() : QStringLiteral("s"));
    if (!recipe.takeOut.empty()) parts << QStringLiteral("took out %1 (%2 shot%3)").arg(recipe.mediaNames.join(QStringLiteral(", "))).arg(named).arg(named == 1 ? QString() : QStringLiteral("s"));
    if (recipe.clearRest) parts << QStringLiteral("cleared everything else off the timeline");
    if (repeats > 0) parts << QStringLiteral("%1 repeat%2 out").arg(repeats).arg(plural(repeats, QString(), QStringLiteral("s")));
    if (!doubled.empty() && !recipe.clearRest) parts << QStringLiteral("%1 doubled title%2 out").arg(doubled.size()).arg(doubled.size() == 1 ? QString() : QStringLiteral("s"));
    if (weakOut > 0) parts << QStringLiteral("%1 weak shot%2 out").arg(weakOut).arg(plural(weakOut, QString(), QStringLiteral("s")));
    if (trimmed > 0) parts << QStringLiteral("%1 shot%2 %3").arg(trimmed).arg(plural(trimmed, QString(), QStringLiteral("s")), growing ? QStringLiteral("lengthened") : QStringLiteral("trimmed"));
    if (placing) parts << QStringLiteral("music added");
    else if (recipe.music == LocalRecipe::Music::off && songOnTimeline) parts << QStringLiteral("music out");
    if (snap && !musicGoes && song && music) parts << QStringLiteral("cut to the beat");
    if (graded > 0) parts << (common == QStringLiteral("none") ? QStringLiteral("looks off") : QStringLiteral("one look (%1)").arg(common.value_or(QString())));
    if (recipe.frame != LocalRecipe::Frame::unchanged && size) parts << frameName;
    if (!levels.empty()) parts << levelPart(levels);
    return Outcome{sequential(p, edits), parts.join(QStringLiteral(", ")), notes, focused, std::nullopt};
}

} // namespace er::LocalImprover
