// Leaf value types of the project model (Project.swift, Captions.swift, MediaFolders.swift,
// MagicHistory.swift, Transcripts.swift). Pure data: no GUI, no engine.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <optional>
#include <vector>

namespace er {

using Uuid = QUuid;
inline Uuid newUuid() { return QUuid::createUuid(); }

/// Every Codable struct keeps the keys it doesn't know, so a file from a newer Mac build (or with
/// fields LX doesn't use) is written back unchanged. Not part of the Mac model.
using Extra = QJsonObject;

struct RGBA {
    double r = 0, g = 0, b = 0, a = 0;
    Extra extra;
    static RGBA white() { return {1, 1, 1, 1, {}}; }
    static RGBA black() { return {0, 0, 0, 1, {}}; }
    bool operator==(const RGBA&) const = default;
};

enum class TrackKind { video, audio };
enum class ClipKind { video, audio, image, solid, text };
enum class MediaKind { video, audio, image };
enum class FadeCurve { linear, easeIn, easeOut, smooth };
enum class TransitionKind { dissolve, dipToBlack, wipe };

inline TrackKind trackKindOf(ClipKind k) { return k == ClipKind::audio ? TrackKind::audio : TrackKind::video; }
inline bool usesSource(ClipKind k) { return k == ClipKind::video || k == ClipKind::audio; }

QString fadeCurveLabel(FadeCurve c);
/// Maps fade progress (0 = silent/transparent, 1 = full) through the curve.
double applyFadeCurve(FadeCurve c, double x);
QString transitionKindLabel(TransitionKind k);

// MARK: Captions (Captions.swift)

/// How every caption looks. One style for the whole track.
struct CaptionStyle {
    /// Point size relative to a 1080px frame's shorter side.
    double size = 64;
    /// Where the captions sit, as a fraction down the frame (their middle).
    double y = 0.78;
    /// The word being said lights up.
    bool highlight = true;
    RGBA textColor = RGBA::white();
    RGBA highlightColor = {1, 0.84, 0.25, 1, {}};
    /// A dark edge around the letters, so they read over anything.
    bool outline = true;
    RGBA outlineColor = RGBA::black();
    Extra extra;
    static constexpr double sizeMin = 28.0, sizeMax = 140.0;
    bool operator==(const CaptionStyle&) const = default;
};

/// The caption track: one per sequence, above the video tracks.
struct CaptionTrack {
    bool hidden = false;
    CaptionStyle style;
    Extra extra;
    bool operator==(const CaptionTrack&) const = default;
};

/// A change to one word of a file's transcript, made on a caption: new text, or "" to leave it out.
struct CaptionFix {
    /// The word's start in the file, which identifies it.
    double start = 0;
    QString text;
    /// Joined to the word before: its time goes to that word, whose text says them both.
    std::optional<bool> merged;
    Extra extra;
    bool operator==(const CaptionFix&) const = default;
};

/// What's said in a file: each word with its time in the file (Transcripts.swift).
struct Transcript {
    struct Word {
        double start = 0, end = 0;
        QString text;
        bool operator==(const Word&) const = default;
    };
    std::vector<Word> words;
    QString language;
    bool isEmpty() const { return words.empty(); }
    QString text() const;
    struct Span { double start, end; };
    struct Sentence { double start, end; QString text; };
    struct Snippet { QString before, match, after; };
    struct Occurrence { double start, end; QString context; };
    /// Stretches of talking: words closer than `gap` seconds run together.
    std::vector<Span> speech(double gap = 0.45) const;
    /// Sentences (or runs of talking, if there's no punctuation), with their times.
    std::vector<Sentence> sentences(double gap = 0.8) const;
    /// Whether they say these words, in this order (case and punctuation aside). With `typing`, the last word
    /// can be the start of one ("gu" finds "guys"), for searching as it's typed.
    bool says(const QString& phrase, bool typing = false) const;
    /// A few words either side of where they say this, for showing why a clip matched.
    std::optional<Snippet> snippet(const QString& phrase, bool typing = false) const;
    /// Every place they say these words, with their times in the file and a few words around.
    std::vector<Occurrence> occurrences(const QString& phrase) const;
    static QStringList tokens(const QString& s);
    bool operator==(const Transcript&) const = default;
};

// MARK: Markers, history, folders

struct Marker {
    enum class Tint { white, yellow, green, blue, purple, red };
    Uuid id = newUuid();
    double time = 0;
    QString name;
    Tint tint = Tint::yellow;
    Extra extra;
    static quint32 hex(Tint t);
    bool operator==(const Marker&) const = default;
};

/// One prompt sent with Magic (MagicHistory.Entry).
struct MagicHistoryEntry {
    QString text;
    /// The Magic tab it was sent from (claude, chatgpt, local).
    QString tab;
    /// Seconds since 2001-01-01 00:00:00 UTC: Swift's default `Date` encoding (deferredToDate).
    double date = 0;
    Extra extra;
    bool operator==(const MagicHistoryEntry&) const = default;
};

/// A folder on disk brought into the project whole (MediaFolders.swift).
struct MediaFolder {
    Uuid id = newUuid();
    QString path;
    /// `path` as seen from the project file's folder, written on save (as for media).
    std::optional<QString> relativePath;
    /// macOS bookmark data. LX can't resolve it; it is kept and written back untouched.
    std::optional<QByteArray> bookmark;
    Extra extra;
    QString name() const;
    bool operator==(const MediaFolder&) const = default;
};

} // namespace er
