// MagicMentions.swift: `@playhead`, `@selected`, `@in-out`, `@` + a library file's name, `@clip:` + one clip on the
// timeline and `@marker:` + a marker (or a marker's name alone), typed in the Magic box to say exactly what a
// request is about; MagicPointers (what's pointed at when Magic starts); and MagicTimes, the times typed in a
// request. Ported literally; ranges are UTF-16 positions, as QString's.
#pragma once
#include "model/Project.h"

namespace er {

class EditorModel;

struct MagicPointers {
    double playhead = 0;
    /// Selected clips, in timeline order.
    std::vector<Uuid> selected;
    std::optional<Range> range;
    static MagicPointers now(const EditorModel& model);
};

struct MagicMentionFound;

struct MagicMention {
    enum class Kind { playhead, selected, range, media, clip, marker };
    Kind kind = Kind::playhead;
    Uuid id;
    bool operator==(const MagicMention&) const = default;

    struct ClipLabel { Uuid id; QString name; ClipKind kind; QString track; double start; };
    struct MarkerLabel { Uuid id; QString name; double time; Marker::Tint tint; };
    using Found = MagicMentionFound;

    static std::vector<MarkerLabel> markerLabels(const Project& p);
    static std::vector<ClipLabel> clipLabels(const Project& p);
    /// A time as a clock ("0:12.4", "1:02:03.5") that MagicTimes reads back as the same place.
    static QString clock(double t);
    static QString markersAsTimes(const QString& text, const Project& p);
    /// Every mention in the text, in order. A file's name counts with or without its extension; the longest wins.
    static std::vector<Found> find(const QString& text, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips = {},
                                   const std::vector<MarkerLabel>& markers = {});
    /// The request in plain words: "@selected" reads "the selected clips", "@beach.mov" reads "beach.mov".
    static QString plain(const QString& text, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips = {},
                         const std::vector<MarkerLabel>& markers = {});
};

struct MagicMentionFound { int at, length; MagicMention mention; };

/// Where a track sits for sorting: video tracks top to bottom, then audio.
int trackOrder(const Project& p, const Uuid& id);

namespace MagicTimes {
struct Typed { int at, length; QString text; double seconds; bool unitOnly; };
std::vector<Typed> times(const QString& text, double fps);
struct Span { double start, end; QString text; };
std::optional<Span> span(const QString& text, double fps);
/// Times Magic will read as places in the edit, for showing them in bold.
std::vector<std::pair<int, int>> recognised(const QString& text);
} // namespace MagicTimes

} // namespace er
