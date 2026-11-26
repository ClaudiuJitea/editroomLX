// AudioScrubber.swift: audio scrubbing that answers instantly. The edit's mixed sound is rendered into memory
// in the background whenever the audio changes; scrubbing then plays straight out of that memory through a
// low-latency device callback, so sound starts within one audio buffer of the mouse moving. The mix is the
// same AudioPlan and Mixer as playback, so gains, fades, mute/solo and ducking are all heard.
#pragma once
#include "AudioPlan.h"
#include <atomic>
#include <memory>

namespace er {

class AudioScrubber {
public:
    /// How long each scrub position keeps sounding if the mouse stops.
    static constexpr double snippet = 0.08;
    static constexpr double rate = 32000.0;
    /// Longer edits aren't held in memory.
    static constexpr double maxSeconds = 45.0 * 60;

    AudioScrubber();
    ~AudioScrubber();
    /// Starts the audio output ahead of time, so the first scrub isn't delayed.
    void warmUp();
    void shutDown();
    /// Renders the mix into memory when the edit's audio has changed since last time (`fingerprint` says
    /// what the audio is; the render runs on a worker thread).
    void prepare(const Project& project, const AudioSources& media, size_t fingerprint);
    /// Plays from a time on the timeline. Each call restarts the short burst from the new place.
    void play(double time);
    bool ready() const;
    /// The mix at a time, for checking: peak level over a short stretch, 0…1.
    float peak(double time, double seconds = 0.05) const;
    /// Waits for a render under way (tests).
    void waitReady(int ms) const;

    struct Impl;
private:
    std::unique_ptr<Impl> d;
};

} // namespace er
