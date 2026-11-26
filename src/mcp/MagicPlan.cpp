#include "MagicPlan.h"
#include <cmath>
#include <numeric>

namespace er {

Project MagicPlan::result() const {
    Project p = base;
    for (const auto& t : tasks) t.edit.apply(p);
    return p;
}

namespace {
MagicEdit edit(MagicEdit::Kind k) { MagicEdit e; e.kind = k; return e; }
MagicEdit fade(bool in, const Uuid& clip, double to) {
    MagicEdit e = edit(in ? MagicEdit::Kind::fadeIn : MagicEdit::Kind::fadeOut);
    e.clip = clip;
    e.to = to;
    return e;
}
MagicEdit gain(const Uuid& clip, double from, double to) {
    MagicEdit e = edit(MagicEdit::Kind::gain);
    e.clip = clip;
    e.from = from;
    e.to = to;
    return e;
}
MagicEdit grade(const Uuid& clip, MagicGradeKey key, double to) {
    MagicEdit e = edit(MagicEdit::Kind::grade);
    e.clip = clip;
    e.key = key;
    e.to = to;
    return e;
}
} // namespace

namespace Choreographer {

std::optional<MagicPlan> plan(const Project& project, const EditDecision& decision) {
    if (decision.shots.empty()) return std::nullopt;
    Project base = project;
    base.clips.clear();
    base.inPoint.reset();
    base.outPoint.reset();
    while (base.videoTracks.size() < 2) base.videoTracks.push_back(Track(TrackKind::video));
    while (base.audioTracks.size() < size_t(decision.music ? 3 : 2)) base.audioTracks.push_back(Track(TrackKind::audio));
    for (auto& t : base.videoTracks) t.locked = false;
    for (auto& t : base.audioTracks) t.locked = false;
    const Uuid v1 = base.videoTracks[0].id, v2 = base.videoTracks[1].id;
    const Uuid a1 = base.audioTracks[0].id, a2 = base.audioTracks[1].id;
    const double fps = base.fps;
    auto frames = [fps](double t) { return std::round(t * fps) / fps; };
    SeededRandom rng(uint64_t(decision.shots.size() * 7919 + project.assets.size()));

    // Where each shot ends up on V1: cuts on whole frames, as close to the plan (the beat) as can be.
    const auto& shots = decision.shots;
    const size_t n = shots.size();
    std::vector<double> finalStart, finalLength;
    double t = 0;
    for (const auto& s : shots) {
        const double a = frames(t), b = frames(t + s.duration);
        finalStart.push_back(a);
        finalLength.push_back(std::max(1 / fps, b - a));
        t += s.duration;
    }
    const double total = frames(t);

    // Handles: shots go down long and get trimmed, with what the file has either side.
    std::vector<double> head, tail;
    for (size_t i = 0; i < n; ++i) {
        const auto& s = shots[i];
        double h = 0.35 + rng.next() * 0.45, tl = 0.35 + rng.next() * 0.5;
        if (s.asset.kind == MediaKind::video) {
            h = std::min(h, s.source);
            tl = std::min(tl, s.asset.duration - s.source - finalLength[i]);
        }
        head.push_back(std::max(0.0, std::floor(h * fps) / fps));
        tail.push_back(std::max(0.0, std::floor(tl * fps) / fps));
    }

    // The rough cut: the same shots, long, shuffled, one after another on V2.
    std::vector<size_t> rough(n);
    std::iota(rough.begin(), rough.end(), 0);
    for (size_t i = n - 1; i > 0; --i) std::swap(rough[i], rough[size_t(rng.next() * double(i + 1))]);
    std::vector<double> roughStart(n, 0.0);
    t = 0;
    for (size_t i : rough) { roughStart[i] = t; t += head[i] + finalLength[i] + tail[i]; }

    std::vector<MagicTask> tasks;
    auto add = [&](MagicEdit e, std::vector<int> after = {}, double delay = 0) {
        tasks.push_back({std::move(e), std::move(after), delay});
        return int(tasks.size()) - 1;
    };

    // Music first, so it's under everything from the start.
    if (decision.music) {
        const auto& m = *decision.music;
        Clip c(base.audioTracks[2].id, ClipKind::audio, 0, frames(m.duration));
        c.assetID = m.asset.id;
        c.sourceStart = m.source;
        const int p = add(MagicEdit::place({c}, m.asset.id));
        add(fade(true, c.id, 0.3), {p}, 0.1);
        add(fade(false, c.id, std::min(m.fadeOut, m.duration / 3)), {p}, 0.2);
        add(gain(c.id, 0, -2), {p}, 0.4);
    }

    struct Cut { Uuid video; std::optional<Uuid> audio; int moved = 0; double roughFrom = 0; };
    std::vector<Cut> cuts(n);
    for (size_t order = 0; order < n; ++order) {
        const size_t i = rough[order];
        const auto& s = shots[i];
        const double r = roughStart[i];
        const double len = head[i] + finalLength[i] + tail[i];
        Clip v(v2, s.asset.kind == MediaKind::image ? ClipKind::image : ClipKind::video, r, len);
        v.assetID = s.asset.id;
        v.sourceStart = s.asset.kind == MediaKind::image ? 0 : s.source - head[i];
        std::vector<Clip> group{v};
        if (s.asset.kind == MediaKind::video && s.asset.hasAudio) {
            const Uuid link = newUuid();
            group[0].linkID = link;
            Clip au(a2, ClipKind::audio, r, len);
            au.assetID = s.asset.id;
            au.sourceStart = v.sourceStart;
            au.linkID = link;
            group.push_back(au);
        }
        std::vector<Uuid> ids;
        for (const auto& c : group) ids.push_back(c.id);
        const int place = add(MagicEdit::place(group, s.asset.id), {}, double(order) * 0.09 + rng.next() * 0.25);
        std::vector<int> ready{place};
        const double headAt = r + head[i];
        if (head[i] > 0.2 && i % 3 == 1) {
            // Cut the head off with the blade and delete it.
            MagicEdit b = edit(MagicEdit::Kind::blade);
            b.ids = ids;
            b.at = headAt;
            for (const auto& id : ids) b.pieces[id] = newUuid();
            b.link = newUuid();
            const auto pieces = b.pieces;
            const int blade = add(b, {place});
            MagicEdit rm = edit(MagicEdit::Kind::remove);
            rm.ids = ids;
            const int remove = add(rm, {blade}, 0.03);
            for (auto& id : ids) id = pieces.at(id);
            ready = {blade, remove};
        } else if (head[i] > 0.5 / fps) {
            MagicEdit th = edit(MagicEdit::Kind::trimHead);
            th.ids = ids;
            th.from = r;
            th.to = headAt;
            ready.push_back(add(th, {place}));
        }
        if (tail[i] > 0.5 / fps) {
            MagicEdit tt = edit(MagicEdit::Kind::trimTail);
            tt.ids = ids;
            tt.from = r + len;
            tt.to = headAt + finalLength[i];
            ready.push_back(add(tt, ready, 0.02));
        }
        MagicEdit mv = edit(MagicEdit::Kind::move);
        mv.ids = ids;
        mv.from = headAt;
        mv.to = finalStart[i];
        mv.tracks[ids[0]] = v1;
        if (ids.size() > 1) mv.tracks[ids[1]] = a1;
        const int moved = add(mv, ready);
        cuts[i] = Cut{ids[0], ids.size() > 1 ? std::optional<Uuid>(ids[1]) : std::nullopt, moved, headAt};
    }

    // A title on V2 once the rough cut has cleared from under it (only one they asked for).
    if (!decision.title.isEmpty()) {
        const double titleLength = std::min(3.0, std::max(2.0, total * 0.12));
        const double titleStart = decision.titleAtEnd ? frames(std::max(0.0, total - titleLength)) : 0;
        std::vector<int> clearing;
        for (const auto& c : cuts) if (decision.titleAtEnd || c.roughFrom < titleLength) clearing.push_back(c.moved);
        Clip title(v2, ClipKind::text, titleStart, frames(titleLength));
        title.text.string = QString();
        title.text.size = 130;
        const int placed = add(MagicEdit::place({title}, std::nullopt), clearing);
        MagicEdit ty = edit(MagicEdit::Kind::type);
        ty.clip = title.id;
        ty.text = decision.title;
        const int typed = add(ty, {placed});
        add(fade(true, title.id, 0.4), {typed});
        add(fade(false, title.id, 0.5), {typed}, 0.05);
    }

    for (size_t i = 0; i < n; ++i) {
        const auto& c = cuts[i];
        const auto& s = shots[i];
        // Sound: the shot's level, with little fades on and off.
        if (c.audio) {
            if (s.gainDB != 0) add(gain(*c.audio, 0, s.gainDB), {c.moved}, 0.03);
            add(i % 2 == 0 ? fade(true, *c.audio, 0.2) : fade(false, *c.audio, 0.25), {c.moved}, 0.08);
        }
        // Dissolves picked from the right-click menu.
        if (i > 0 && s.dissolve) {
            MagicEdit tr = edit(MagicEdit::Kind::transition);
            tr.clip = c.video;
            tr.transition = Transition{TransitionKind::dissolve, 0.5, {}};
            add(tr, {c.moved, cuts[i - 1].moved}, 0.1);
        }
        // The last shot fades to black.
        if (i == n - 1) add(fade(false, c.video, 0.6), {c.moved}, 0.12);
        // Colour only where it's wanted: the look they asked for (with a little punch), and exposure for a shot
        // clearly darker or brighter than the rest.
        if (!decision.look.isEmpty()) {
            MagicEdit lk = edit(MagicEdit::Kind::look);
            lk.clip = c.video;
            lk.text = decision.look;
            add(lk, {c.moved}, 0.1);
            add(grade(c.video, MagicGradeKey::contrast, 12), {c.moved}, 0.15);
            add(grade(c.video, MagicGradeKey::saturation, 18), {c.moved}, 0.175);
        }
        if (std::abs(s.exposure) >= 0.2) add(grade(c.video, MagicGradeKey::exposure, s.exposure), {c.moved}, 0.125);
    }
    return MagicPlan{base, tasks};
}

} // namespace Choreographer
} // namespace er
