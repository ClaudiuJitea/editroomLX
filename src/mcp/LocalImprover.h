// LocalImprover.swift: Local Magic on a timeline that already has an edit makes that edit better instead of
// starting over, with no AI. It keeps the person's shots, order and music, and takes out repeats and the weakest
// shots, trims long shots to their best moment, puts cuts on the beat and ends on a bar, closes the gaps, gives
// every shot one look, evens out exposure and sets the sound. Ported literally.
#pragma once
#include "MagicPlan.h"

namespace er::LocalImprover {

struct Outcome {
    MagicPlan plan;
    /// What changed, in a sentence, for the log and the note.
    QString summary;
    /// What it couldn't do of what was asked.
    QStringList notes;
    /// It did just what was asked (named media), rather than a general improvement.
    bool focused = false;
    /// The whole reply, when there's nothing to summarise.
    std::optional<QString> message;
};

std::optional<Outcome> plan(const Project& p, const std::map<Uuid, ClipAnalysis>& analyses, const std::optional<MusicAnalysis>& music,
                            const LocalRecipe& recipe = {}, const std::optional<MediaAsset>& newSong = std::nullopt);

/// A plan of edits made one after another (MCPDraft.plan).
MagicPlan sequential(const Project& base, const std::vector<MagicEdit>& edits);

} // namespace er::LocalImprover
