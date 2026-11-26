#include "Meters.h"
#include <cmath>

namespace er {

double MeterState::db(double linear) { return linear > 0 ? std::max(MeterReading::floorDB, 20 * std::log10(linear)) : MeterReading::floorDB; }

void MeterState::update(const Project& project, const WaveformFor& waveforms, double t, bool playing, double now) {
    // Fast: a reading follows the sound almost at once and falls away in a few frames.
    release_ = 4;
    peakFall_ = 2;
    stopped_ = !playing;
    std::map<Uuid, MeterReading> next;
    double power = 0, powerL = 0, powerR = 0;
    for (const auto& track : project.audioTracks) {
        double level = 0, levelL = 0, levelR = 0;
        if (playing && project.isAudible(track)) {
            for (const auto& c : project.clips) {
                if (c.trackID != track.id || c.kind != ClipKind::audio || !(t >= c.start && t < c.end()) || !c.assetID) continue;
                const Waveform* wf = waveforms ? waveforms(*c.assetID) : nullptr;
                if (!wf || wf->peaks.empty()) continue;
                // The loudest peak over the last ~30ms of source.
                const double src = c.sourceTime(t);
                const int hi = std::min(int(wf->peaks.size()) - 1, int(src * wf->peaksPerSecond));
                const int lo = std::max(0, int((src - 0.03 * c.speedAtSource(src)) * wf->peaksPerSecond));
                if (hi < lo) continue;
                float peak = 0, pl = 0, pr = 0;
                for (int i = lo; i <= hi; ++i) {
                    peak = std::max(peak, wf->peaks[size_t(i)]);
                    if (size_t(i) < wf->left.size()) pl = std::max(pl, wf->left[size_t(i)]);
                    if (size_t(i) < wf->right.size()) pr = std::max(pr, wf->right[size_t(i)]);
                }
                const double g = dbToLinear(c.gainDB) * c.fadeGain(t);
                level = std::max(level, double(peak) * g);
                levelL = std::max(levelL, double(pl) * g);
                levelR = std::max(levelR, double(pr) * g);
            }
            const double tg = dbToLinear(track.gainDB);
            level *= tg; levelL *= tg; levelR *= tg;
        }
        power += level * level;
        powerL += levelL * levelL;
        powerR += levelR * levelR;
        auto it = tracks_.find(track.id);
        next[track.id] = advance(it != tracks_.end() ? it->second : MeterReading(), db(level), track.id.toString(), now);
    }
    auto sum = [](double p) { return p > 0 ? 10 * std::log10(p) : MeterReading::floorDB; };
    master_ = advance(master_, sum(power), QStringLiteral("master"), now);
    left_ = advance(left_, sum(powerL), QStringLiteral("L"), now);
    right_ = advance(right_, sum(powerR), QStringLiteral("R"), now);
    tracks_ = next;
}

MeterReading MeterState::advance(const MeterReading& old, double reading, const QString& key, double now) {
    if (stopped_) return MeterReading();
    MeterReading r = old;
    r.level = std::max(reading, std::max(MeterReading::floorDB, old.level - release_));
    if (reading >= old.peak) {
        r.peak = reading;
        holdUntil_[key] = now + peakHold_;
    } else if (now > holdUntil_[key]) {
        r.peak = std::max(MeterReading::floorDB, old.peak - peakFall_);
    }
    r.clipped = reading >= -0.1;
    return r;
}

} // namespace er
