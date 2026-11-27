// AudioStream (AudioStream.swift): the player's sound. The audio clock is the master: the picture shows
// whatever time is being heard. Each track is its own lane, rendered ahead (0.2 s) on a feeder thread and
// mixed in the device callback with the lane's volume, so muting and soloing are heard at once. Looping
// wraps the feed at the loop's end, so it's gapless. Output through miniaudio (PipeWire/PulseAudio, ALSA).
#pragma once
#include "Mixer.h"
#include <atomic>
#include <memory>

namespace er {

class AudioStream {
public:
    AudioStream();
    ~AudioStream();
    /// Whether an output device opened (headless machines have none: the clock then runs on its own).
    bool hasDevice() const;
    QString deviceName() const;

    void setPlan(const AudioPlan& plan);
    void setAudible(const std::map<Uuid, bool>& audible);
    void setLoop(std::optional<std::pair<double, double>> span);
    void play(double from);
    void pause();
    void seek(double t);
    bool isPlaying() const;
    /// The timeline time being heard now: what was handed to the output, less what's still on its way to the ears
    /// (`outputLatency`).
    double currentTime() const;
    /// Set when the feed reached the end of the timeline (not looping) and everything queued was heard.
    bool ended() const;
    /// Set once each time a loop went round; cleared by reading.
    bool takeWrap();
    /// How long sound handed over takes to be heard: the device's buffer plus the output's own delay.
    double outputLatency() const;
    /// The output's own delay as the sound server reports it (Bluetooth headphones: 0.15–0.3 s).
    double sinkLatency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace er
