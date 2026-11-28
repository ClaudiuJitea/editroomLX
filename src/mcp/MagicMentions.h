// MagicMentions.swift: `@playhead`, `@selected`, `@in-out`, `@` + a library file's name, `@clip:` + one clip on the
// timeline, `@marker:` + a marker (or a marker's name alone) and `@folder:` + a folder in Media (`:only` for just its
// own files), typed in the Magic box to say exactly what a request is about; MagicPointers (what's pointed at when Magic starts); and MagicTimes, the times typed in a
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
    /// `folder`: a folder in the Media panel, by its path: every file in it and in the folders inside it
    /// (`@folder:Drone`), or with `only`, just the files directly in it (`@folder:Drone:only`).
    enum class Kind { playhead, selected, range, media, clip, marker, folder };
    Kind kind = Kind::playhead;
    Uuid id;
    QString path;
    bool only = false;
    bool operator==(const MagicMention&) const = default;
    static MagicMention folder(const QString& path, bool only) { MagicMention m; m.kind = Kind::folder; m.path = path; m.only = only; return m; }

    struct ClipLabel { Uuid id; QString name; ClipKind kind; QString track; double start; };
    struct MarkerLabel { Uuid id; QString name; double time; Marker::Tint tint; };
    /// A folder in Media as `@folder:` names it: its own name, or its path in Media ("B-roll / Drone") when another
    /// folder has the same name. `files` are what's in it and in the folders inside it; `direct` just what's directly
    /// in it.
    struct FolderLabel {
        QString path, name, label;
        std::vector<Uuid> files, direct;
        /// Folders inside it hold some of its files: `@folder:…:only` is offered for just its own.
        bool hasSubfolders() const { return !direct.empty() && direct.size() < files.size(); }
        /// What a request means by it: everything in it, or only what's directly in it (`:only`, or the request says so).
        std::vector<Uuid> filesFor(const QString& request, bool only = false) const;
    };
    using Found = MagicMentionFound;

    static std::vector<FolderLabel> folderLabels(const Project& p);
    /// The request asks for only what's directly in a folder, not the folders inside it ("only the clips in this
    /// folder, not the sub-folders", "top level only").
    static bool onlyDirect(const QString& request);
    static std::vector<MarkerLabel> markerLabels(const Project& p);
    static std::vector<ClipLabel> clipLabels(const Project& p);
    /// A time as a clock ("0:12.4", "1:02:03.5") that MagicTimes reads back as the same place.
    static QString clock(double t);
    static QString markersAsTimes(const QString& text, const Project& p);
    /// Every mention in the text, in order. A file's name counts with or without its extension; the longest wins.
    static std::vector<Found> find(const QString& text, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips = {},
                                   const std::vector<MarkerLabel>& markers = {}, const std::vector<FolderLabel>& folders = {});
    /// The request in plain words: "@selected" reads "the selected clips", "@beach.mov" reads "beach.mov".
    static QString plain(const QString& text, const std::vector<MediaAsset>& media, const std::vector<ClipLabel>& clips = {},
                         const std::vector<MarkerLabel>& markers = {}, const std::vector<FolderLabel>& folders = {});
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
