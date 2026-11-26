// Local's small, direct edits, each ported literally from its Swift file: LocalPointed (what's selected, the
// playhead, the in/out range, moving shots by name, adding a shot of what a file shows), LocalTitle, LocalCaptions,
// LocalAdd, LocalRamp, LocalSpeech (cutting the pauses) and LocalCombined (several in one request).
#pragma once
#include "MagicMentions.h"
#include "MagicPlan.h"
#include "analysis/LocalBrain.h"

namespace er {

/// LocalPointed.Outcome: the plan (none when there's nothing it can do) and what to say.
struct LocalOutcome {
    std::optional<MagicPlan> plan;
    QString said;
};

using TranscriptOf = std::function<std::optional<Transcript>(const Uuid& asset)>;

namespace LocalPointed {
std::optional<LocalOutcome> plan(const QString& request, const LocalRecipe& recipe, const MagicPointers& ptr, const Project& p);
std::optional<LocalOutcome> addShown(const QString& request, const LocalRecipe& recipe, const Project& p, std::optional<double> playhead);
std::optional<LocalOutcome> reorder(const QString& request, const Project& p);
} // namespace LocalPointed

namespace LocalTitle {
bool asks(const QString& request, const Project& p);
bool newCut(const QString& request);
LocalOutcome plan(const QString& request, double playhead, const Project& p);
} // namespace LocalTitle

namespace LocalCaptions {
bool asks(const QString& request, const Project& p);
IdSet targets(const MagicPointers& ptr, const Project& p);
LocalOutcome plan(const QString& request, const MagicPointers& ptr, const Project& p, const TranscriptOf& transcript);
} // namespace LocalCaptions

namespace LocalAdd {
bool asks(const QString& request, const Project& p);
int count(const QString& request);
LocalOutcome plan(const QString& request, const QString& typed, const MagicPointers& ptr, const Project& p);
} // namespace LocalAdd

namespace LocalRamp {
bool asks(const QString& request);
bool atCuts(const QString& request);
LocalOutcome plan(const QString& request, const MagicPointers& ptr, const Project& p);
} // namespace LocalRamp

/// LocalZoom.swift: Local's zooms, as the zoom button on Scale makes them: "zoom in on @selected", "add a zoom",
/// "push in", "zoom out", "zoom in and out", or "remove the zoom". On the clips pointed at or selected, else the
/// one at the playhead.
namespace LocalZoom {
bool asks(const QString& request);
LocalOutcome plan(const QString& request, const MagicPointers& ptr, const Project& p);
} // namespace LocalZoom

namespace LocalSpeech {
bool asks(const QString& request);
LocalOutcome plan(const Project& p, const std::optional<IdSet>& only, const TranscriptOf& transcript);
} // namespace LocalSpeech

namespace LocalCombined {
enum class Part { add, title, captions };
std::vector<Part> parts(const QString& request, const Project& p);
/// Each in turn on the edit as the one before leaves it. (Transcripts for captions are heard before this.)
LocalOutcome plan(const std::vector<Part>& parts, const QString& request, const QString& typed, const MagicPointers& ptr, const Project& p,
                  const TranscriptOf& transcript);
} // namespace LocalCombined

} // namespace er
