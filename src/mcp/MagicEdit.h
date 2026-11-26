// MagicEdit (MagicPlan.swift): one edit an agent or Local Magic makes, applied to a project exactly as
// the Mac applies it at the end of its cursor animation (`apply(_, 1)`).
#pragma once
#include "model/Project.h"
#include <functional>
#include <variant>

namespace er {

enum class MagicGradeKey { exposure, contrast, saturation };

struct MagicEdit {
    enum class Kind { place, trimHead, trimTail, move, blade, remove, change, type, gain, fadeIn, fadeOut, look, grade, transition };
    Kind kind = Kind::change;
    std::vector<Clip> clips;               // place
    std::optional<Uuid> asset;             // place
    std::vector<Uuid> ids;                 // trim/move/blade/remove
    double from = 0, to = 0;               // trim/move/gain
    std::map<Uuid, Uuid> tracks;           // move
    double at = 0;                         // blade
    std::map<Uuid, Uuid> pieces;           // blade
    std::optional<Uuid> link;              // blade
    bool ripple = false;                   // remove
    std::optional<Uuid> clip;              // change/type/gain/fade/look/grade/transition
    QString label;                         // change
    std::function<void(Project&)> body;    // change
    QString text;                          // type, look name
    MagicGradeKey key = MagicGradeKey::exposure;
    Transition transition;

    static MagicEdit place(std::vector<Clip> clips, std::optional<Uuid> asset);
    static MagicEdit change(std::optional<Uuid> clip, QString label, std::function<void(Project&)> body);
    void apply(Project& p) const;
};

} // namespace er
