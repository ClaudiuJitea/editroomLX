// The mixer: an AudioPlan rendered to interleaved stereo float. It replaces AVFoundation's composition
// audio, AVAudioMix and the `.spectral` time-pitch algorithm: each voice reads its file (resampled once,
// to the mix rate), pieces whose speed isn't 1 go through Signalsmith Stretch (pitch kept), the lane's
// volume envelope is applied sample by sample, and lanes are summed with their own volume (mute and solo,
// heard at once).
//
// Rendering is stateful per voice, so consecutive calls (playback, export) read on without seeking.
#pragma once
#include "AudioPlan.h"
#include <memory>

namespace er {

class Mixer {
public:
    explicit Mixer(int rate = 48000);
    ~Mixer();
    void setPlan(const AudioPlan& plan);
    const AudioPlan& plan() const;
    int rate() const;
    /// Lane volumes (0 silences a lane without rebuilding: mute and solo).
    void setLaneGains(const std::map<Uuid, double>& gains);
    /// `frames` stereo frames of the mix from timeline time `t0`, appended to `out`.
    void render(double t0, int frames, std::vector<float>& out);
    /// One lane's sound alone (for the meters and tests).
    void renderLane(size_t lane, double t0, int frames, std::vector<float>& out);
    /// The whole mix of [t0, t1), rendered in parallel by lane.
    std::vector<float> renderRange(double t0, double t1);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace er
