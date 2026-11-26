#include "AutoPlanner.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace er {

QString LocalRecipe::summary() const {
    QStringList p;
    if (seconds) p << QStringLiteral("%1s").arg(int(std::round(*seconds)));
    if (pace != Pace::unchanged) p << (pace == Pace::fast ? QStringLiteral("fast") : QStringLiteral("calm"));
    if (look) p << *look;
    if (music != Music::unchanged) p << (music == Music::off ? QStringLiteral("no music") : QStringLiteral("add music"));
    if (frame != Frame::unchanged) p << (frame == Frame::vertical ? QStringLiteral("vertical") : frame == Frame::square ? QStringLiteral("square") : QStringLiteral("horizontal"));
    if (!keepOnly.empty()) p << QStringLiteral("only ") + mediaNames.join(QStringLiteral(", "));
    if (!takeOut.empty()) p << QStringLiteral("without ") + mediaNames.join(QStringLiteral(", "));
    if (speed) p << QStringLiteral("%1x").arg(*speed);
    if (soundEffects) p << QStringLiteral("sound effects");
    if (levelAudio) p << QStringLiteral("level audio");
    if (title) p << (title->isEmpty() ? QStringLiteral("title") : QStringLiteral("title \"%1\"").arg(*title));
    if (wholeLibrary) p << QStringLiteral("whole library");
    if (shots) p << QStringLiteral("%1 shots").arg(*shots);
    if (every) p << QStringLiteral("every clip");
    return p.isEmpty() ? QStringLiteral("plain") : p.join(QStringLiteral(", "));
}

namespace AutoPlanner {

namespace {
struct Window { double start, score; };
}

std::optional<EditDecision> decide(const Project& /*project*/, const std::vector<std::pair<MediaAsset, ClipAnalysis>>& clips,
                                   const std::optional<std::pair<MediaAsset, MusicAnalysis>>& music, const LocalRecipe& recipe) {
    std::vector<std::pair<MediaAsset, ClipAnalysis>> usable;
    for (const auto& c : clips) if (!c.second.samples.empty()) usable.push_back(c);
    if (usable.empty()) return std::nullopt;

    std::vector<ClipAnalysis::Sample> all;
    for (const auto& c : usable) all.insert(all.end(), c.second.samples.begin(), c.second.samples.end());
    const auto score = scorer(all);
    auto pct = [](const std::vector<double>& v, double p) { return percentile(v, p); };

    struct Source {
        MediaAsset asset;
        ClipAnalysis analysis;
        std::vector<double> scores;
        std::vector<std::pair<double, double>> picked;
    };
    // In the order things were filmed; undated files keep their library order, last.
    std::vector<std::pair<int, Source>> indexed;
    for (size_t i = 0; i < usable.size(); ++i) {
        Source s{usable[i].first, usable[i].second, {}, {}};
        for (const auto& x : s.analysis.samples) s.scores.push_back(score(x));
        indexed.push_back({int(i), std::move(s)});
    }
    std::stable_sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) {
        const auto& x = a.second.analysis.created;
        const auto& y = b.second.analysis.created;
        if (x && y) return *x < *y;
        if (x && !y) return true;
        if (!x && y) return false;
        return a.first < b.first;
    });
    std::vector<Source> sources;
    for (auto& [i, s] : indexed) sources.push_back(std::move(s));
    std::vector<double> allScores;
    for (const auto& s : sources) allScores.insert(allScores.end(), s.scores.begin(), s.scores.end());
    const double floor = pct(allScores, 0.3);

    // Shot lengths: whole beats with music, a natural rhythm without.
    std::optional<double> period;
    if (music) period = music->second.period();
    // Every clip asked for: at least one shot from each. When that many won't fit the length on whole beats,
    // shots go to half beats.
    std::optional<int> wanted = recipe.every ? std::optional<int>(std::max(recipe.shots.value_or(0), int(sources.size()))) : recipe.shots;
    int sub = 1;
    const double pace = recipe.pace == LocalRecipe::Pace::fast ? 0.55 : recipe.pace == LocalRecipe::Pace::calm ? 1.7 : 1;
    int longBeats = 4, shortBeats = 2;
    if (period) {
        longBeats = 16;
        for (int b : {2, 4, 8, 16}) if (double(b) * *period >= 1.5 * pace) { longBeats = b; break; }
        shortBeats = double(longBeats / 2) * *period >= 0.8 ? longBeats / 2 : longBeats;
    }
    std::optional<double> perShot;
    if (wanted) {
        const int n = *wanted;
        const double length = recipe.seconds.value_or(double(n) * (recipe.pace == LocalRecipe::Pace::calm ? 2.5 : recipe.pace == LocalRecipe::Pace::fast ? 0.9 : 1.5));
        perShot = std::max(recipe.every ? 0.2 : 0.35, length / double(n));
        if (recipe.every && period && *perShot < *period * 0.9) {
            sub = 2;
            period = *period / 2;
        }
        if (period) {
            const int b = std::max(1, int(std::floor(*perShot / *period)));
            longBeats = b;
            shortBeats = b;
        }
    }
    static const bool rhythm[6] = {true, false, false, true, true, false};
    auto beatsFor = [&](int k) { return rhythm[k % 6] ? longBeats : shortBeats; };
    auto lengthFor = [&](int k) {
        static const double per[6] = {1.08, 0.94, 1.0, 1.06, 0.92, 1.0};
        static const double natural[6] = {2.4, 1.8, 2.1, 2.8, 1.7, 2.2};
        if (period) return double(beatsFor(k)) * *period;
        if (perShot) return *perShot * per[k % 6];
        return natural[k % 6] * pace;
    };

    // How long: about half the good footage, between 12 and 40 seconds, and no longer than the song allows.
    double good = 0;
    for (const auto& s : sources) {
        const double step = s.analysis.samples.size() > 1 ? s.analysis.duration / double(s.analysis.samples.size()) : 2.5;
        good += double(std::count_if(s.scores.begin(), s.scores.end(), [&](double v) { return v >= floor; })) * step;
    }
    double target = recipe.seconds ? *recipe.seconds : perShot ? *perShot * double(wanted.value_or(1)) : std::min(40.0, std::max(12.0, good * 0.5));
    if (music) target = std::min(target, music->second.duration - 2);

    /// The best window of `length` in a source that doesn't overlap what's already been taken from it.
    auto bestWindow = [&](const Source& s, double length, double apart) -> std::optional<Window> {
        const ClipAnalysis& a = s.analysis;
        if (s.asset.kind == MediaKind::image) {
            if (s.picked.empty()) return Window{0, s.scores.empty() ? 0 : s.scores.front()};
            return std::nullopt;
        }
        const double limit = std::min(a.duration, s.asset.duration);
        if (!(limit >= length + 0.1)) return std::nullopt;
        std::optional<Window> best, bestWanted;
        static const std::vector<double> none;
        auto mit = recipe.moments.find(s.asset.id);
        const std::vector<double>& wantedAt = mit == recipe.moments.end() ? none : mit->second;
        double start = 0.05;
        while (start + length <= limit - 0.05) {
            const double end = start + length;
            const bool clear = std::all_of(s.picked.begin(), s.picked.end(), [&](const auto& p) { return end + apart <= p.first || start >= p.second + apart; });
            if (clear) {
                std::vector<double> vals;
                for (size_t i = 0; i < a.samples.size(); ++i) if (a.samples[i].time >= start && a.samples[i].time <= end) vals.push_back(s.scores[i]);
                if (!vals.empty()) {
                    const double v = std::accumulate(vals.begin(), vals.end(), 0.0) / double(vals.size()) * 0.75 + *std::min_element(vals.begin(), vals.end()) * 0.25;
                    if (!best || v > best->score) best = Window{start, v};
                    const double edge = std::min(0.3, length / 4);
                    if (std::any_of(wantedAt.begin(), wantedAt.end(), [&](double t) { return t >= start + edge && t <= end - edge; })
                        && (!bestWanted || v > bestWanted->score)) bestWanted = Window{start, v};
                }
            }
            start += 0.25;
        }
        return bestWanted ? bestWanted : best;
    };

    std::vector<EditDecision::Shot> shots;
    std::vector<int> shotBeats;
    double total = 0;
    int round = 0;
    while (total < target) {
        int added = 0;
        for (size_t i = 0; i < sources.size(); ++i) {
            if (!(total < target || (recipe.every && round == 0))) continue;
            // A second shot from a clip only if it's long and the moment is well away from the first.
            if (round > 0 && std::min(sources[i].analysis.duration, sources[i].asset.duration) < 12) continue;
            const double apart = round > 0 ? 4.0 : 0.3;
            double length = lengthFor(int(shots.size()));
            int beats = beatsFor(int(shots.size()));
            auto found = bestWindow(sources[i], length + 0.08, apart);
            if (!found && (beats != shortBeats || !period)) {
                beats = shortBeats;
                length = period ? double(shortBeats) * *period : std::min(1.6, perShot.value_or(1.6));
                found = bestWindow(sources[i], length + 0.08, apart);
            }
            if (!found) continue;
            const Window w = *found;
            if (!((recipe.every && round == 0) || w.score >= (round == 0 || total < 18 ? pct(allScores, 0.12) : floor))) continue;
            // Not two shots from one clip in a row.
            if (!shots.empty() && shots.back().asset.id == sources[i].asset.id && sources.size() > 1) continue;
            sources[i].picked.push_back({w.start, w.start + length});
            std::vector<double> luma;
            for (const auto& x : sources[i].analysis.samples) if (x.time >= w.start && x.time <= w.start + length) luma.push_back(x.luma);
            const double m = luma.empty() ? 0.45 : std::accumulate(luma.begin(), luma.end(), 0.0) / double(luma.size());
            const double exposure = std::round(std::clamp(std::log2(0.45 / std::max(0.05, m)) * 0.5, -0.5, 0.5) * 10) / 10;
            shots.push_back({sources[i].asset, w.start, length, exposure, 0, false, w.score});
            shotBeats.push_back(beats);
            total += length;
            ++added;
        }
        if (added == 0) break;
        ++round;
        if (recipe.every) break;
    }
    // Short of the length: shots grow a beat (or a quarter second) at a time, in turn, as far as their clips allow.
    {
        const double limit = target;
        const double unit = period.value_or(0.25);
        bool grew = true;
        while (total + unit <= limit + 0.05 && grew) {
            grew = false;
            for (size_t i = 0; i < shots.size(); ++i) {
                if (!(total + unit <= limit + 0.05)) continue;
                const auto& s = shots[i];
                double analysed = s.asset.duration;
                for (const auto& src : sources) if (src.asset.id == s.asset.id) { analysed = src.analysis.duration; break; }
                if (!(s.asset.kind == MediaKind::video && s.source + s.duration + unit <= std::min(s.asset.duration, analysed) - 0.05)) continue;
                const double end = s.source + s.duration + unit;
                bool clear = true;
                for (size_t j = 0; j < shots.size(); ++j)
                    if (j != i && shots[j].asset.id == s.asset.id && !(end + 0.1 <= shots[j].source || shots[j].source + shots[j].duration <= s.source)) clear = false;
                if (!clear) continue;
                shots[i].duration += unit;
                shotBeats[i] += 1;
                total += unit;
                grew = true;
            }
        }
    }
    // Every clip in a length they gave: past it, extra shots come off the end, then the last shot is cut down.
    if (recipe.every && recipe.seconds) {
        const double limit = *recipe.seconds;
        while (total > limit + 0.15 && shots.size() > sources.size()) {
            total -= shots.back().duration;
            shots.pop_back();
            shotBeats.pop_back();
        }
        if (total > limit + 0.15 && !shots.empty()) {
            const size_t last = shots.size() - 1;
            if (period) {
                while (total > limit + 0.15 && shotBeats[last] > 1) {
                    shotBeats[last] -= 1;
                    shots[last].duration -= *period;
                    total -= *period;
                }
            } else {
                const double cut = std::min(total - limit, shots[last].duration - 0.3);
                shots[last].duration -= std::max(0.0, cut);
                total -= std::max(0.0, cut);
            }
        }
    }
    if (shots.empty()) return std::nullopt;

    // A title only when they asked for one: their words, or "Title" to type over.
    const QString asked = recipe.title ? (recipe.title->isEmpty() ? QStringLiteral("Title") : *recipe.title) : QString();
    EditDecision decision;
    decision.shots = shots;
    decision.title = asked;
    decision.look = recipe.look ? (*recipe.look == QStringLiteral("none") ? QString() : *recipe.look) : QString();
    decision.titleAtEnd = recipe.titleAtEnd;

    if (music && !music->second.beats.empty()) {
        const MediaAsset& asset = music->first;
        const MusicAnalysis& m = music->second;
        decision.bpm = m.bpm;
        std::vector<double> grid = m.beats;
        std::vector<float> energy = m.energy;
        if (sub == 2) {
            grid.clear();
            for (size_t i = 0; i < m.beats.size(); ++i) {
                grid.push_back(m.beats[i]);
                if (i + 1 < m.beats.size()) grid.push_back((m.beats[i] + m.beats[i + 1]) / 2);
            }
            energy.clear();
            for (float e : m.energy) { energy.push_back(e); energy.push_back(e); }
        }
        const int bar = 4 * sub, phase = m.barPhase * sub;
        const double step = m.period() / double(sub);
        const int count = std::accumulate(shotBeats.begin(), shotBeats.end(), 0);
        const int extra = recipe.every && recipe.seconds ? 0 : (bar - count % bar) % bar;
        if (extra > 0 && !decision.shots.empty()) {
            const size_t last = decision.shots.size() - 1;
            const auto s = decision.shots[last];
            const double needed = s.duration + double(extra) * step;
            auto fits = [&](double start) {
                if (s.asset.kind != MediaKind::video) return true;
                if (!(start >= 0 && start + needed <= s.asset.duration - 0.05)) return false;
                for (size_t j = 0; j < decision.shots.size(); ++j)
                    if (j != last && decision.shots[j].asset.id == s.asset.id && !(start + needed + 0.1 <= decision.shots[j].source || start >= decision.shots[j].source + decision.shots[j].duration + 0.1))
                        return false;
                return true;
            };
            std::optional<double> start;
            for (double c : {s.source, s.source - (needed - s.duration)}) if (fits(c)) { start = c; break; }
            if (start) {
                shotBeats[last] += extra;
                decision.shots[last].source = *start;
            } else if (shotBeats[last] > bar - extra) {
                shotBeats[last] -= bar - extra;
            } else if (shotBeats[last] == bar - extra && last > 0 && !recipe.every) {
                shotBeats.pop_back();
                decision.shots.pop_back();
            }
        }
        int totalBeats = std::accumulate(shotBeats.begin(), shotBeats.end(), 0);
        // Drop shots from the end until the song is long enough.
        while (totalBeats + 2 >= int(grid.size()) && shotBeats.size() > 1) {
            totalBeats -= shotBeats.back();
            shotBeats.pop_back();
            decision.shots.pop_back();
        }
        // Start the song on a bar where it's lively and lifting.
        int bestBeat = phase;
        double bestScore = -INFINITY;
        const int look = 8 * sub;
        auto meanOf = [&](int from, int to) {
            double s = 0;
            for (int i = from; i < to; ++i) s += energy[size_t(i)];
            return s;
        };
        for (int b = phase; b + totalBeats + 1 < int(grid.size()) && b + totalBeats <= int(energy.size()); b += bar) {
            const double mean = meanOf(b, b + totalBeats) / double(std::max(1, totalBeats));
            const double before = b >= look ? meanOf(b - look, b) / double(look) : mean;
            const int ahead = std::min(int(energy.size()), b + look);
            const double after = meanOf(b, ahead) / double(std::max(1, std::min(look, int(energy.size()) - b)));
            const double sc = mean + 0.5 * std::max(0.0, after - before) - (b == phase && b < bar ? 0.05 : 0);
            if (sc > bestScore) { bestScore = sc; bestBeat = b; }
        }
        const int b0 = bestBeat;
        // Cut exactly on the song's beats.
        int at = b0;
        for (size_t i = 0; i < decision.shots.size(); ++i) {
            const int next = at + shotBeats[i];
            if (next >= int(grid.size()) || at >= int(grid.size())) break;
            decision.shots[i].duration = grid[size_t(next)] - grid[size_t(at)];
            decision.shots[i].dissolve = sub == 1 && i > 0 && (at - b0) % 16 == 0;
            at = next;
        }
        at = std::min(at, int(grid.size()) - 1);
        const double length = grid[size_t(at)] - grid[size_t(b0)];
        const double tail = std::min(1.2, std::max(0.0, m.duration - grid[size_t(at)]));
        decision.music = EditDecision::Music{asset, grid[size_t(b0)], length + tail, tail + 0.6};
        for (auto& s : decision.shots) if (s.asset.hasAudio) s.gainDB = -16;
    } else {
        for (size_t i = 0; i < decision.shots.size(); ++i) decision.shots[i].dissolve = i > 0 && i % 4 == 0;
    }
    // Keep shots inside their files after any lengthening.
    for (auto& s : decision.shots)
        if (s.asset.kind == MediaKind::video) s.source = std::max(0.0, std::min(s.source, s.asset.duration - s.duration - 0.02));
    return decision;
}

} // namespace AutoPlanner
} // namespace er
