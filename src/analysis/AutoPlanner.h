// AutoPlanner.swift and LocalBrain.swift's LocalRecipe: what the cut should be, before anything is put on the
// timeline (which moments, how long, in what order, over which part of the song), with no network and no key.
// Ported literally.
#pragma once
#include "MediaAnalyzer.h"
#include "model/Project.h"
#include <map>
#include <set>

namespace er {

/// What Local Magic should do, worked out from what the person typed. Local's own planner follows it.
struct LocalRecipe {
    enum class Pace { unchanged, calm, fast };
    enum class Music { unchanged, off, add };
    enum class Frame { unchanged, vertical, horizontal, square };
    std::optional<double> seconds;
    Pace pace = Pace::unchanged;
    /// A built-in look for every shot; "none" takes looks off; none leaves colour alone.
    std::optional<QString> look;
    Music music = Music::unchanged;
    Frame frame = Frame::unchanged;
    QString cannotDo;
    std::set<Uuid> keepOnly, takeOut;
    QStringList mediaNames;
    bool clearRest = false;
    /// Where the things they asked for show in each file: its shots are taken from around these times.
    std::map<Uuid, std::vector<double>> moments;
    std::vector<Uuid> shown;
    QString asked;
    bool wholeLibrary = false;
    std::optional<int> shots;
    bool every = false;
    bool onlyAsked = false;
    std::optional<double> speed;
    bool soundEffects = false;
    bool askedJudgement = false;
    /// None if they didn't ask for a title; "" for "Title" to type over; else their words.
    std::optional<QString> title;
    bool titleAtEnd = false;
    bool levelAudio = false;
    std::map<Uuid, double> levelGains;
    QStringList levelSilent;

    bool isEmpty() const {
        return !seconds && pace == Pace::unchanged && !look && music == Music::unchanged && frame == Frame::unchanged && keepOnly.empty()
            && takeOut.empty() && !speed && !soundEffects && !levelAudio && !title && !wholeLibrary && !shots;
    }
    bool onlyLevels() const {
        return levelAudio && !seconds && pace == Pace::unchanged && music == Music::unchanged && frame == Frame::unchanged && keepOnly.empty()
            && takeOut.empty() && !speed && !soundEffects;
    }
    bool isFocused() const { return !keepOnly.empty() || !takeOut.empty() || onlyAsked; }
    /// "20s, fast, Mono, no music, vertical", for the log and the note.
    QString summary() const;
};

/// What the cut should be. The choreographer turns it into edits.
struct EditDecision {
    struct Shot {
        MediaAsset asset;
        double source = 0, duration = 0;
        double exposure = 0;
        double gainDB = 0;
        bool dissolve = false;
        double score = 0;
    };
    struct Music {
        MediaAsset asset;
        double source = 0, duration = 0, fadeOut = 0;
    };
    std::vector<Shot> shots;
    std::optional<Music> music;
    QString title, look;
    std::optional<double> bpm;
    bool titleAtEnd = false;
};

namespace AutoPlanner {
std::optional<EditDecision> decide(const Project& project, const std::vector<std::pair<MediaAsset, ClipAnalysis>>& clips,
                                   const std::optional<std::pair<MediaAsset, MusicAnalysis>>& music, const LocalRecipe& recipe = {});
} // namespace AutoPlanner

} // namespace er
