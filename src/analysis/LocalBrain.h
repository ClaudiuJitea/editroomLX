// LocalBrain.swift: turns what someone typed in Magic's Local tab into a LocalRecipe that Local's own planner
// follows. Its word helpers (shot counts, speed, titles, quotes, levels, named media, what files show) are the
// Mac's, literally. The Mac asks Apple's on-device model for the settings it reads (length, pace, look, music,
// frame, keep or remove named files); Linux has no such model, so LX reads those with rules built from the same
// word lists the Mac checks the model's answer against (`checked`). On this computer, nothing downloaded.
#pragma once
#include "AutoPlanner.h"
#include "model/SceneTags.h"

namespace er::LocalBrain {

using Shows = std::function<std::optional<SceneTags>(const Uuid&)>;

extern const QStringList looks;
bool isGeneral(const QString& request);
bool asksJudgement(const QString& request);
std::optional<int> shotCount(const QString& request);
bool wantsEvery(const QString& request);
bool wantsLibrary(const QString& request);
std::optional<QString> titleAsked(const QString& request);
std::optional<QString> quotedWords(const QString& request);
bool wantsSoundEffects(const QString& request);
bool wantsLevels(const QString& request);
std::vector<MediaAsset> soundEffects(const Project& p);
std::optional<double> speed(const QString& request);
/// `folder`: a folder was named with @folder:, so "all" means all of that folder, not those as well as the rest.
void applyMedia(LocalRecipe& r, const std::vector<MediaAsset>& named, const QString& action, const QString& request,
                const std::set<Uuid>& pointed = {}, bool hasEdit = false, bool folder = false);
LocalRecipe checked(const LocalRecipe& r, const QString& request);
std::vector<MediaAsset> named(const QString& request, const std::vector<MediaAsset>& media);
std::vector<MediaAsset> showing(const QStringList& words, const std::vector<MediaAsset>& media, const Shows& shows);
QString askedPhrase(const QString& request, const QStringList& matched);
std::map<Uuid, std::vector<double>> moments(const QStringList& words, const std::vector<MediaAsset>& media, const Shows& shows);
/// LocalTitle.atEnd and LocalCaptions.wantedWithCut, which the recipe reads.
bool titleAtEnd(const QString& request);
bool captionsRemoved(const QString& request);
bool captionsWithCut(const QString& request);
/// Reads a request into a recipe, with everything the Mac's popover adds after its model's reading.
LocalRecipe recipe(const QString& request, const std::vector<MediaAsset>& media, const std::set<Uuid>& pointed, bool hasEdit, const Shows& shows,
                   bool folder = false);

} // namespace er::LocalBrain
