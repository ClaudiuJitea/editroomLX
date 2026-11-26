// AudioPlan: the audio side of CompositionBuilder.build, ported. Each audible track becomes a lane: its
// clips as pieces of source sound (a ramped clip in many short pieces, one per step through each ramp),
// and a volume envelope built exactly as the Mac builds its AVAudioMix: a set volume at each clip's start,
// curved fades as 16 linear pieces, and ducking with fades as 50 ms linear pieces.
#pragma once
#include "model/Project.h"
#include <QString>
#include <functional>
#include <map>

namespace er {

/// AVMutableAudioMixInputParameters' volume: set points and linear ramps; between them the volume holds
/// the last value set (1 before anything).
class VolumeEnvelope {
public:
    void set(double t, double v) { events_.push_back({t, t, v, v}); }
    void ramp(double t0, double t1, double v0, double v1) { events_.push_back({t0, t1, v0, v1}); }
    /// Sorts the events (call once, after building).
    void finish();
    double at(double t) const;
    size_t size() const { return events_.size(); }
private:
    struct E { double t0, t1, v0, v1; };
    std::vector<E> events_;
};

struct AudioPiece {
    /// Timeline seconds and the source seconds they play (in the file the piece reads).
    double t0 = 0, t1 = 0, s0 = 0, s1 = 0;
    double rate() const { return (s1 - s0) / std::max(1e-12, t1 - t0); }
};

struct AudioVoice {
    Uuid clip;
    Uuid asset;
    /// The file the sound is read from (the reversed copy for a clip playing backwards).
    QString path;
    bool reversed = false;
    std::vector<AudioPiece> pieces;
    double start() const { return pieces.empty() ? 0 : pieces.front().t0; }
    double end() const { return pieces.empty() ? 0 : pieces.back().t1; }
};

struct AudioLane {
    Uuid track;
    std::vector<AudioVoice> voices;
    VolumeEnvelope volume;
};

struct AudioPlan {
    std::vector<AudioLane> lanes;
    double duration = 0;
    uint64_t signature = 0;
    bool isEmpty() const { return lanes.empty(); }
};

/// What the audio side needs from the media store.
struct AudioSources {
    /// The file a clip's sound is read from, and its sound's time range ([start, end) in file seconds).
    std::function<std::optional<std::pair<QString, std::pair<double, double>>>(const Uuid& asset)> sound;
    /// The reversed copy of a file's sound and its length, once made (nullopt: not yet).
    std::function<std::optional<std::pair<QString, double>>(const Uuid& asset)> reversed;
    /// Waveform peaks (per second, values) for ducking; nullopt when there's no waveform yet.
    std::function<const std::vector<float>*(const Uuid& asset, double* peaksPerSecond)> peaks;
};

namespace AudioLevels {
/// Stretches of time where a track has sound, for ducking another track under it.
std::vector<std::pair<double, double>> activity(const Uuid& track, const Project& project, const AudioSources& media);
/// The ducking gain at a time: down by `low` while there's activity, easing down just before it starts
/// and back up after it stops.
double duckGain(double t, const std::vector<std::pair<double, double>>& spans, double low);
/// A clip's typical level in dB, from the peaks over its source range, ignoring near-silence.
std::optional<double> envelopeDB(const Clip& c, const std::vector<float>& peaks, double peaksPerSecond);
std::optional<double> peakDB(const Clip& c, const std::vector<float>& peaks, double peaksPerSecond);
} // namespace AudioLevels

namespace AudioPlanBuilder {
/// The composition's time grid (CompositionBuilder.timescale): times are rounded to 1/60000 s.
double quantize(double seconds);
/// `keepAllTracks` (the player): muted and unsoloed tracks keep their lane at their own level; the
/// player silences them itself, at once.
AudioPlan build(const Project& project, const AudioSources& media, bool keepAllTracks = false);
}

} // namespace er
