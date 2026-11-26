// MediaAnalyzer.swift: what a video or photo looks like over time (brightness, focus, motion, colour) and
// the beat of a song (spectral-flux onsets, tempo by autocorrelation favouring common tempos, a fitted grid,
// bar starts from the bass), measured on this computer, cached by file path, size and date. Plus
// AutoPlanner's frame scorer and best moments, which analyze_media reports.
//
// Faces: Vision on the Mac, YuNet here (engine/Faces.h). Apple's aesthetics score and "utility" (a screenshot or
// a document): CLIP here, once the scene model is downloaded (SceneTagStore::aesthetics); until then the term
// takes the Mac's own "not available" value, as on macOS 14 (docs/PARITY.md).
#pragma once
#include <QString>
#include <functional>
#include <optional>
#include <vector>

namespace er {

struct ClipAnalysis {
    struct Sample {
        double time = 0;
        /// Average brightness, 0…1.
        double luma = 0;
        /// Detail: variance of edges. Blurry or out-of-focus frames are low.
        double sharpness = 0;
        /// Change since the previous sample: shaky or fast-panning stretches are high.
        double motion = 0;
        /// How colourful, 0…1-ish.
        double color = 0;
        int faces = 0;
        std::optional<double> aesthetic;
        std::optional<bool> utility;
    };
    double duration = 0;
    std::optional<double> created; // seconds since 2001-01-01, as Swift's Date
    std::vector<Sample> samples;
};

struct MusicAnalysis {
    double duration = 0, bpm = 0;
    std::vector<double> beats;
    /// Index into `beats` of the first beat of a bar; bars are four beats.
    int barPhase = 0;
    /// Loudness of each beat, 0…1.
    std::vector<float> energy;
    double period() const { return 60 / bpm; }
    bool isBarStart(int beat) const { return ((beat - barPhase) % 4 + 4) % 4 == 0; }
};

namespace MediaAnalyzer {
inline constexpr int version = 2;
std::optional<ClipAnalysis> clip(const QString& path, bool isImage, int orientation = 1);
std::optional<MusicAnalysis> music(const QString& path);
/// The beat tracker, on mono samples at `rate`.
std::optional<MusicAnalysis> beats(const std::vector<float>& x, double rate);
} // namespace MediaAnalyzer

namespace AutoPlanner {
double percentile(std::vector<double> v, double p);
std::function<double(const ClipAnalysis::Sample&)> scorer(const std::vector<ClipAnalysis::Sample>& all);
struct Moment { double start, end, score; int faces; };
std::vector<Moment> moments(const ClipAnalysis& a, double limit, double length, int count,
                            const std::function<double(const ClipAnalysis::Sample&)>& score);
} // namespace AutoPlanner

} // namespace er
