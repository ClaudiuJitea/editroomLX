#include "MagicEdit.h"
#include <cmath>

namespace er {

MagicEdit MagicEdit::place(std::vector<Clip> clips, std::optional<Uuid> asset) {
    MagicEdit e;
    e.kind = Kind::place;
    e.clips = std::move(clips);
    e.asset = asset;
    return e;
}

MagicEdit MagicEdit::change(std::optional<Uuid> clip, QString label, std::function<void(Project&)> body) {
    MagicEdit e;
    e.kind = Kind::change;
    e.clip = clip;
    e.label = std::move(label);
    e.body = std::move(body);
    return e;
}

void MagicEdit::apply(Project& project) const {
    auto each = [&](const std::vector<Uuid>& list, const std::function<void(Clip&)>& f) {
        for (const auto& id : list) if (auto i = project.index(id)) f(project.clips[*i]);
    };
    auto idSet = [](const std::vector<Uuid>& v) { return IdSet(v.begin(), v.end()); };
    switch (kind) {
    case Kind::place: {
        project.clips.insert(project.clips.end(), clips.begin(), clips.end());
        // Dropped on something: it overwrites it, as on the timeline.
        IdSet ids;
        for (const auto& c : clips) ids.insert(c.id);
        project.resolveOverlaps(ids);
        break;
    }
    case Kind::trimHead: {
        const double t = project.snapFrame(to);
        each(ids, [&](Clip& c) { c.trimHead(t); });
        project.resolveOverlaps(idSet(ids));
        break;
    }
    case Kind::trimTail: {
        const double t = project.snapFrame(to);
        each(ids, [&](Clip& c) { c.trimTail(t); });
        project.resolveOverlaps(idSet(ids));
        break;
    }
    case Kind::move: {
        const double delta = project.snapFrame(to) - from;
        each(ids, [&](Clip& c) {
            c.start += delta;
            if (auto it = tracks.find(c.id); it != tracks.end()) c.trackID = it->second;
        });
        project.resolveOverlaps(idSet(ids));
        break;
    }
    case Kind::blade:
        for (const auto& id : ids) {
            auto i = project.index(id);
            auto piece = pieces.find(id);
            if (!i || piece == pieces.end()) continue;
            Clip right = project.clips[*i];
            if (!(at > right.start && at < right.end())) continue;
            right.id = piece->second;
            right.linkID = project.clips[*i].linkID ? link : std::nullopt;
            right.fadeIn = 0;
            right.trimHead(at);
            project.clips[*i].fadeOut = 0;
            project.clips[*i].trimTail(at);
            project.clips.push_back(right);
        }
        break;
    case Kind::remove: project.remove(idSet(ids), ripple); break;
    case Kind::change: if (body) body(project); break;
    case Kind::type: if (clip) each({*clip}, [&](Clip& c) { c.text.string = text; }); break;
    case Kind::gain: if (clip) each({*clip}, [&](Clip& c) { c.gainDB = std::round(to * 10) / 10; }); break;
    case Kind::fadeIn: if (clip) each({*clip}, [&](Clip& c) { c.fadeIn = to; c.clampFades(); }); break;
    case Kind::fadeOut: if (clip) each({*clip}, [&](Clip& c) { c.fadeOut = to; c.clampFades(); }); break;
    case Kind::look:
        if (clip) each({*clip}, [&](Clip& c) { ColorGrade g = c.grade.value_or(ColorGrade()); g.look = text; c.grade = g; });
        break;
    case Kind::grade:
        if (clip) each({*clip}, [&](Clip& c) {
            ColorGrade g = c.grade.value_or(ColorGrade());
            if (key == MagicGradeKey::exposure) g.exposure = to;
            else if (key == MagicGradeKey::contrast) g.contrast = to;
            else g.saturation = to;
            c.grade = g;
        });
        break;
    case Kind::transition: if (clip) each({*clip}, [&](Clip& c) { c.transitionIn = transition; }); break;
    }
}

} // namespace er
