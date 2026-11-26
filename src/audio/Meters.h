// Meters.swift: track and output meter readings from the waveform peaks at the playhead (no hook into
// the audio itself), with the Mac's ballistics. Values are dBFS.
#pragma once
#include "AudioPlan.h"
#include "Waveform.h"
#include <map>

namespace er {

struct MeterReading {
    static constexpr double floorDB = -60.0;
    double level = floorDB;
    double peak = floorDB;
    /// Lit only while the signal is actually at full scale.
    bool clipped = false;
    bool differs(const MeterReading& o) const { return std::abs(level - o.level) > 0.2 || std::abs(peak - o.peak) > 0.2 || clipped != o.clipped; }
};

class MeterState {
public:
    using WaveformFor = std::function<const Waveform*(const Uuid& asset)>;
    void update(const Project& project, const WaveformFor& waveforms, double t, bool playing, double now);
    const std::map<Uuid, MeterReading>& tracks() const { return tracks_; }
    const MeterReading& master() const { return master_; }
    const MeterReading& left() const { return left_; }
    const MeterReading& right() const { return right_; }

private:
    MeterReading advance(const MeterReading& old, double reading, const QString& key, double now);
    static double db(double linear);
    std::map<Uuid, MeterReading> tracks_;
    MeterReading master_, left_, right_;
    double release_ = 0.9;
    const double peakHold_ = 0.5;
    double peakFall_ = 0.45;
    std::map<QString, double> holdUntil_;
    bool stopped_ = false;
};

} // namespace er
