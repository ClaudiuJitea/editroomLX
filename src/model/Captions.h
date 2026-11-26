// Captions.swift: caption blocks and CaptionLayout, ported literally.
#pragma once
#include "Project.h"
#include <functional>

namespace er {

/// One caption: a few words shown together.
struct CaptionBlock {
    struct Word {
        /// Timeline times.
        double start = 0, end = 0;
        QString text;
        /// The word's start in its file, for fixes.
        double source = 0;
        /// The words in the file it stands for (more than one when a retyped caption has new words).
        std::vector<double> covers;
        bool operator==(const Word&) const = default;
    };
    Uuid clipID;
    Uuid assetID;
    double start = 0, end = 0;
    std::vector<Word> words;

    /// The clip it comes from and its first word's time in the file: steady through trims and moves.
    QString id() const;
    QString text() const;
    /// The words in the file the caption stands for, in order.
    std::vector<double> covered() const;
    bool operator==(const CaptionBlock&) const = default;
};

namespace CaptionLayout {
/// Longest phrase, in characters, and longest time on screen.
inline constexpr int maxChars = 26;
inline constexpr double maxSeconds = 2.6;
/// A pause this long starts a new caption.
inline constexpr double pause = 0.5;

using TranscriptFor = std::function<std::optional<Transcript>(const MediaAsset&)>;
std::vector<CaptionBlock> blocks(const Project& project, const TranscriptFor& transcript);
} // namespace CaptionLayout

/// Swift `String.count`: grapheme clusters, not UTF-16 units.
int graphemeCount(const QString& s);

} // namespace er
