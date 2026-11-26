// Project.swift (+ the Clip extensions in SpeedRamp.swift, the Project extensions in Captions.swift and
// MediaFolders.swift), ported literally. Swift `Set<UUID>` → std::set, `[UUID: Clip]` → std::map.
#pragma once
#include "ColorGrade.h"
#include "SpeedRamp.h"
#include "Types.h"
#include "ZoomMove.h"
#include <functional>
#include <map>
#include <set>

namespace er {

using IdSet = std::set<Uuid>;

struct Track {
    Uuid id = newUuid();
    TrackKind kind = TrackKind::video;
    bool hidden = false;
    bool muted = false;
    double gainDB = 0;
    /// Audio only: while any track is soloed, only soloed tracks are heard.
    bool solo = false;
    /// A locked track's clips can't be selected, moved, trimmed or cut.
    bool locked = false;
    /// Audio only: this track is turned down while the named track has sound, such as music under a voice.
    std::optional<Uuid> duckUnder;
    double duckDB = -12;
    Extra extra;

    Track() = default;
    explicit Track(TrackKind k) : kind(k) {}
    bool operator==(const Track&) const = default;
};

struct TextStyle {
    QString string = QStringLiteral("Text");
    /// Font family; empty means the system font.
    QString fontName;
    bool bold = true;
    /// Point size relative to a 1080px tall frame.
    double size = 110;
    RGBA color = RGBA::white();
    double x = 0.5, y = 0.5;
    Extra extra;
    bool operator==(const TextStyle&) const = default;
};

struct Transition {
    TransitionKind kind = TransitionKind::dissolve;
    /// Seconds; may be shortened to what the clips allow.
    double duration = 0.5;
    Extra extra;
    bool operator==(const Transition&) const = default;
};

/// A stretch of time over which a clip is brought in or taken out by a transition.
struct TransitionEdge {
    double start = 0, end = 0;
    TransitionKind kind = TransitionKind::dissolve;
    double progress(double t) const;
    bool operator==(const TransitionEdge&) const = default;
};

/// How transitions change the way one clip is drawn: shown for longer, brought in, or taken out.
struct TransitionPlan {
    /// Seconds shown before the clip's start, from media before its in point.
    double leadIn = 0.0;
    /// Seconds shown after its end, from media after its out point.
    double tailOut = 0.0;
    std::optional<TransitionEdge> reveal;
    std::optional<TransitionEdge> conceal;
    bool operator==(const TransitionPlan&) const = default;
};

/// The footage a reversed clip was made from.
struct ReverseSource {
    Uuid assetID;
    double sourceStart = 0;
    Extra extra;
    bool operator==(const ReverseSource&) const = default;
};

struct Clip {
    Uuid id = newUuid();
    Uuid trackID;
    ClipKind kind = ClipKind::video;
    std::optional<Uuid> assetID;
    double start = 0;
    double duration = 0;
    double sourceStart = 0;
    double speed = 1;
    double fadeIn = 0;
    double fadeOut = 0;
    std::optional<FadeCurve> fadeInCurve;
    std::optional<FadeCurve> fadeOutCurve;
    double gainDB = 0;
    std::optional<Uuid> linkID;
    RGBA color = {0.16, 0.16, 0.2, 1, {}};
    TextStyle text;
    /// Colour correction for visual clips; nil means untouched.
    std::optional<ColorGrade> grade;
    /// Size and placement for video, image and solid clips; nil means fitted to the frame.
    std::optional<ClipTransform> transform;
    /// A transition into this clip from the clip that ends where it starts, on the same track.
    std::optional<Transition> transitionIn;
    /// A freeze frame: shows the single frame at `sourceStart` for its whole length.
    std::optional<bool> freeze;
    /// Set on a reversed clip: where its footage came from, so reversing again restores it.
    std::optional<ReverseSource> reversedFrom;
    /// Plays its source backwards. Source time `s` is time `length - s` in the file.
    std::optional<bool> reversed;
    /// The speed changing across the clip; nil plays at `speed` throughout.
    std::optional<SpeedRamp> ramp;
    /// A sound clip whose speech shows on the caption track.
    std::optional<bool> captioned;
    Extra extra;

    Clip() = default;
    /// Swift's memberwise `Clip(trackID:kind:start:duration:)`, with the common trailing options.
    Clip(Uuid track, ClipKind k, double s, double d, double srcStart = 0, double spd = 1)
        : trackID(track), kind(k), start(s), duration(d), sourceStart(srcStart), speed(spd) {}

    bool isFreeze() const { return freeze == true; }
    bool playsBackwards() const { return reversed == true; }
    /// Reversed either way: playing backwards, or (in older projects) using a reversed copy of a file.
    bool isReversed() const { return playsBackwards() || reversedFrom.has_value(); }
    /// Plays through its source over time (so speed and source limits apply); a freeze frame doesn't.
    bool playsSource() const { return usesSource(kind) && !isFreeze(); }
    /// Clips that can be scaled and moved in the frame (text has its own placement).
    bool isTransformable() const { return kind == ClipKind::video || kind == ClipKind::image || kind == ClipKind::solid; }
    double end() const { return start + duration; }

    /// Level from the fades at a time on the timeline, 0...1.
    double fadeGain(double t) const;
    FadeCurve inCurve() const { return fadeInCurve.value_or(FadeCurve::linear); }
    FadeCurve outCurve() const { return fadeOutCurve.value_or(FadeCurve::linear); }
    void clampFades();
    void trimHead(double t);
    void trimTail(double t);

    // SpeedRamp.swift extension
    bool isRamped() const { return ramp.has_value() && playsSource(); }
    double speedAtSource(double s) const;
    /// The footage showing at timeline time `t` (any time, before or after the clip too).
    double sourceTime(double t) const;
    /// Footage the clip covers.
    double sourceLength() const;
    /// Timeline seconds to play footage from `s0` to `s1`.
    double timelineSpan(double s0, double s1) const;
    std::pair<double, double> speedRange() const;
    /// Changes how it plays while keeping the same footage: its length follows.
    void retime(const std::function<double(double)>& snap, double minimum, const std::function<void(Clip&)>& change);

    bool operator==(const Clip&) const = default;
};

/// Which footage a clip shows at each moment, for the renderer.
struct SourceClock {
    double start = 0, sourceStart = 0, speed = 1;
    std::optional<SpeedRamp> ramp;
    explicit SourceClock(const Clip& clip);
    double source(double t) const;
};

struct MediaAsset {
    Uuid id = newUuid();
    QString path;
    /// `path` as seen from the project file's folder, written on save.
    std::optional<QString> relativePath;
    /// Set by the Mac when the original had to be converted to something AVFoundation can decode.
    /// LX never writes it for new imports; it is kept from Mac projects.
    std::optional<QString> playablePath;
    QString name;
    MediaKind kind = MediaKind::video;
    double duration = 0;
    int width = 0;
    int height = 0;
    double fps = 0;
    bool hasAudio = false;
    /// macOS bookmark data. LX can't resolve it; it is kept and written back untouched.
    std::optional<QByteArray> bookmark;
    Extra extra;
    bool operator==(const MediaAsset&) const = default;
};

namespace TrackToken {
/// Placeholder track ids used while a drag targets a track that doesn't exist yet.
Uuid newVideo();
Uuid newAudio();
} // namespace TrackToken

struct CaptionBlock;

struct Range { double start = 0, end = 0; bool operator==(const Range&) const = default; };
struct TransitionSpan { double start = 0, end = 0; bool fromHead = false; };

struct Project {
    int width = 1920;
    int height = 1080;
    double fps = 30;
    bool settingsLocked = false;
    /// Index 0 is the bottom-most video track (V1).
    std::vector<Track> videoTracks{Track(TrackKind::video), Track(TrackKind::video)};
    /// Index 0 is the top-most audio track (A1).
    std::vector<Track> audioTracks{Track(TrackKind::audio), Track(TrackKind::audio)};
    std::vector<Clip> clips;
    std::vector<MediaAsset> assets;
    /// Remembered so repeat exports don't ask again.
    std::optional<QString> exportName;
    std::optional<QString> exportDir;
    /// Named points on the timeline (optional so older projects open).
    std::optional<std::vector<Marker>> markers;
    /// The in/out range, set with I and O. Either can be set alone.
    std::optional<double> inPoint;
    std::optional<double> outPoint;
    /// Prompts sent with Magic in this project, newest first.
    std::optional<std::vector<MagicHistoryEntry>> magicHistory;
    /// The caption track, when there is one.
    std::optional<CaptionTrack> captions;
    /// Words fixed or left out on captions, by file.
    std::optional<std::map<Uuid, std::vector<CaptionFix>>> captionFixes;
    /// Folders on disk brought in whole, which the Media panel mirrors.
    std::optional<std::vector<MediaFolder>> folders;
    /// Files (or whole folders) in those folders taken out of the project.
    std::optional<std::vector<QString>> hiddenMedia;
    Extra extra;

    bool operator==(const Project&) const = default;

    static constexpr double minClip = 1.0 / 60.0;

    double duration() const;
    double aspect() const { return double(width) / double(std::max(1, height)); }
    double snapFrame(double t) const;

    const MediaAsset* asset(const std::optional<Uuid>& id) const;
    const Clip* clip(const Uuid& id) const;
    const Track* track(const Uuid& id) const;
    bool isLocked(const Uuid& trackID) const;
    /// Drops clips on locked tracks.
    IdSet unlocked(const IdSet& ids) const;
    /// Whether an audio track is heard, taking mute and solo into account.
    bool isAudible(const Track& track) const;
    std::optional<size_t> index(const Uuid& id) const;
    const std::vector<Track>& tracks(TrackKind kind) const { return kind == TrackKind::video ? videoTracks : audioTracks; }
    std::optional<size_t> trackIndex(const Uuid& id, TrackKind kind) const;
    QString name(const Clip& clip) const;
    /// Expands a set of clip ids to include everything linked to them.
    IdSet linked(const IdSet& ids) const;
    std::optional<double> sourceLimit(const Clip& clip) const;
    std::vector<Marker> markerList() const;
    /// The in/out range, if one is set and longer than a frame.
    std::optional<Range> playRange() const;
    /// Just the in/out range, moved to start at zero.
    Project cropped(Range range) const;
    std::vector<double> snapTargets(const IdSet& excluding) const;
    /// Sorted, de-duplicated edit points for playhead jumping.
    std::vector<double> editPoints() const;
    /// Allowed delta range for dragging a clip edge.
    std::pair<double, double> trimLimits(const Clip& c, bool left, const IdSet& excluding) const;
    std::optional<Uuid> freeTrack(TrackKind kind, double start, double duration) const;
    std::optional<std::pair<Uuid, double>> firstFreeVideoSpot(double time, double duration) const;

    // Mutations
    Uuid addTrack(TrackKind kind);
    void removeTrack(const Uuid& id);
    void updateTrack(const Uuid& id, const std::function<void(Track&)>& body);
    void materializeTokens();
    void resolveOverlaps(const IdSet& keeping);
    IdSet split(const IdSet& ids, double t);
    void remove(const IdSet& ids, bool ripple);
    void rippleRemove(double a, double b, const IdSet& tracks);
    void setSpeed(const IdSet& ids, double speed, bool sticky = false);
    void setRamp(const IdSet& ids, double speed, double p1, double p2, double p3, double p4,
                 double start, double end, bool sticky = false);
    void retime(const IdSet& ids, bool sticky, const std::function<void(Clip&)>& change);
    std::map<Uuid, Clip> carried(const std::map<Uuid, Clip>& edited) const;
    bool fits(const std::map<Uuid, Clip>& changed) const;

    // Transitions
    const Clip* previousTouching(const Clip& c) const;
    const Clip* nextTouching(const Clip& c) const;
    std::optional<TransitionSpan> transitionSpan(const Clip& a, const Clip& b) const;
    TransitionPlan transitionPlan(const Clip& c) const;

    // Gaps
    std::optional<Range> gap(const Uuid& track, double t) const;
    double pullLeft(const Uuid& track, double time, double amount);
    void closeAllGaps(const Uuid& track);
    void unlink(const IdSet& ids);
    void fitClips(const IdSet& toMedia);
    void sanitize();

    // Paths (Project.swift / MediaFolders.swift)
    void stampRelativePaths(const QString& projectDir);
    int resolveMovedMedia(const QString& projectDir);
    const MediaFolder* linkedFolder(const QString& path) const;
    void movePaths(const QString& from, const QString& to);
    bool isHidden(const QString& path) const;
    void pruneFolders(const std::set<Uuid>& keeping = {});

    // Captions (Captions.swift)
    std::vector<Clip> captionedClips() const;
    IdSet captionTargets(const IdSet& ids) const;
    IdSet defaultCaptionTargets() const;
    void addCaptions(const IdSet& ids);
    void removeCaptions(const IdSet& ids);
    void removeCaptionTrack();
    void setCaptionText(const CaptionBlock& block, const QString& text);

private:
    double headRoom(const Clip& c) const;
    double tailRoom(const Clip& c) const;
};

/// `db <= -59.5` is silence.
double dbToLinear(double db);
/// "−∞ dB", "+1.5 dB", "−3.0 dB", "0.0 dB" (with a real minus sign).
QString formatDB(double db);

} // namespace er
