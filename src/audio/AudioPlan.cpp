#include "AudioPlan.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <set>

namespace er {

void VolumeEnvelope::finish() {
    std::stable_sort(events_.begin(), events_.end(), [](const E& a, const E& b) { return a.t0 < b.t0; });
}

double VolumeEnvelope::at(double t) const {
    // The last event starting at or before t.
    auto it = std::upper_bound(events_.begin(), events_.end(), t, [](double v, const E& e) { return v < e.t0; });
    if (it == events_.begin()) return 1.0;
    const E& e = *std::prev(it);
    if (t < e.t1 && e.t1 > e.t0) return e.v0 + (e.v1 - e.v0) * (t - e.t0) / (e.t1 - e.t0);
    return e.v1;
}

// MARK: AudioLevels

std::vector<std::pair<double, double>> AudioLevels::activity(const Uuid& trackID, const Project& project, const AudioSources& media) {
    std::vector<std::pair<double, double>> spans;
    const double step = 0.05;
    for (const auto& c : project.clips) {
        if (c.trackID != trackID || c.kind != ClipKind::audio || !c.assetID || !media.peaks) continue;
        double pps = 0;
        const std::vector<float>* peaks = media.peaks(*c.assetID, &pps);
        if (!peaks || peaks->empty()) continue;
        std::optional<double> open;
        double t = c.start;
        while (t < c.end()) {
            const double src = c.sourceTime(t);
            const int lo = std::max(0, int(src * pps));
            const int hi = std::min(int(peaks->size()) - 1, int(c.sourceTime(t + step) * pps));
            float peak = 0;
            if (hi >= lo) for (int i = lo; i <= hi; ++i) peak = std::max(peak, (*peaks)[size_t(i)]);
            const bool active = double(peak) * dbToLinear(c.gainDB) * c.fadeGain(t) > 0.03;
            if (active && !open) open = t;
            if (!active && open) { spans.emplace_back(*open, t); open.reset(); }
            t += step;
        }
        if (open) spans.emplace_back(*open, c.end());
    }
    // Short pauses between words don't let the music back up.
    std::stable_sort(spans.begin(), spans.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> merged;
    for (const auto& s : spans) {
        if (!merged.empty() && s.first - merged.back().second < 0.35) merged.back().second = std::max(merged.back().second, s.second);
        else merged.push_back(s);
    }
    return merged;
}

double AudioLevels::duckGain(double t, const std::vector<std::pair<double, double>>& spans, double low) {
    const double attack = 0.15, release = 0.5;
    double g = 1.0;
    for (const auto& [s, e] : spans) {
        if (t < s - attack || t > e + release) continue;
        double f;
        if (t < s) f = (s - t) / attack;
        else if (t > e) f = (t - e) / release;
        else f = 0;
        g = std::min(g, low + (1 - low) * f);
    }
    return g;
}

std::optional<double> AudioLevels::envelopeDB(const Clip& c, const std::vector<float>& peaks, double pps) {
    if (peaks.empty()) return std::nullopt;
    const int lo = std::max(0, int(c.sourceStart * pps));
    const int hi = std::min(int(peaks.size()), int((c.sourceStart + c.sourceLength()) * pps));
    if (!(hi > lo)) return std::nullopt;
    double sum = 0;
    int n = 0;
    for (int i = lo; i < hi; ++i) {
        if (!(peaks[size_t(i)] > 0.003f)) continue;
        const double v = peaks[size_t(i)];
        sum += v * v;
        ++n;
    }
    if (n == 0) return std::nullopt;
    return 10 * std::log10(sum / n);
}

std::optional<double> AudioLevels::peakDB(const Clip& c, const std::vector<float>& peaks, double pps) {
    if (peaks.empty()) return std::nullopt;
    const int lo = std::max(0, int(c.sourceStart * pps));
    const int hi = std::min(int(peaks.size()), int((c.sourceStart + c.sourceLength()) * pps));
    if (!(hi > lo)) return std::nullopt;
    const float peak = *std::max_element(peaks.begin() + lo, peaks.begin() + hi);
    if (!(peak > 0)) return std::nullopt;
    return 20 * std::log10(double(peak));
}

// MARK: Build

double AudioPlanBuilder::quantize(double s) { return std::round(s * 60000.0) / 60000.0; }

namespace {
using Q = double;
Q time(double s) { return AudioPlanBuilder::quantize(s); }

/// Places one clip's sound (CompositionBuilder.insert). Returns false when it can't go on the lane.
bool insert(const Clip& clip, double rangeStart, double rangeEnd, AudioVoice& voice, double& cursor) {
    const Q start = time(clip.start), end = time(clip.end());
    if (!(start >= cursor) || !(end > start)) return false;
    if (clip.isFreeze()) {
        // One source frame stretched over the whole clip.
        const Q frame = 1.0 / 60;
        Q at = time(clip.sourceStart);
        if (at < rangeStart) at = rangeStart;
        if (at + frame > rangeEnd) at = rangeEnd - frame;
        if (!(at >= rangeStart)) return false;
        voice.pieces.push_back({start, end, at, at + frame});
        cursor = end;
        return true;
    }
    if (clip.isRamped()) {
        // A ramped clip: its footage in short pieces, each played at its own speed.
        const SpeedRamp& ramp = *clip.ramp;
        const double s0 = std::max(rangeStart, clip.sourceStart);
        const double s1 = std::min(rangeEnd, clip.sourceTime(clip.end()));
        if (!(s1 > s0)) return false;
        std::set<double> cutSet{s0, s1};
        for (auto [a, b] : {std::make_pair(ramp.inFrom, ramp.inTo), std::make_pair(ramp.outFrom, ramp.outTo)}) {
            if (!(b > a)) continue;
            const int steps = std::max(4, std::min(90, int(std::ceil(clip.timelineSpan(a, b) * 30))));
            for (int i = 0; i <= steps; ++i) cutSet.insert(a + (b - a) * double(i) / double(steps));
        }
        std::vector<double> cuts;
        for (double c : cutSet) if (c >= s0 && c <= s1) cuts.push_back(c);
        Q at = start;
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const double a = cuts[i], b = cuts[i + 1];
            if (!(b - a > 1e-6)) continue;
            const Q srcA = time(a), srcB = time(b);
            const Q e = time(clip.start + clip.timelineSpan(clip.sourceStart, b));
            if (!(srcB > srcA) || !(e > at)) continue;
            voice.pieces.push_back({at, e, srcA, srcB});
            at = e;
        }
        cursor = at;
        return at > start;
    }
    const bool unity = std::abs(clip.speed - 1) < 1e-9;
    Q srcStart = time(clip.sourceStart);
    Q tgtDur = end - start;
    Q srcDur = unity ? tgtDur : time(clip.duration * clip.speed);
    if (srcStart < rangeStart) srcStart = rangeStart;
    const Q available = rangeEnd - srcStart;
    if (!(available > 0)) return false;
    if (srcDur > available) {
        srcDur = available;
        tgtDur = unity ? available : std::min(tgtDur, time(available / clip.speed));
    }
    if (!(srcDur > 0) || !(tgtDur > 0)) return false;
    voice.pieces.push_back({start, start + tgtDur, srcStart, srcStart + srcDur});
    cursor = start + tgtDur;
    return true;
}

/// Volume ramps are linear, so curved fades are approximated with short linear pieces.
void fadeRamp(VolumeEnvelope& env, double start, double duration, double gain, FadeCurve curve, bool rising) {
    const int pieces = curve == FadeCurve::linear ? 1 : 16;
    double t0 = start;
    for (int i = 0; i < pieces; ++i) {
        const double x0 = double(i) / pieces, x1 = double(i + 1) / pieces;
        const double t1 = i == pieces - 1 ? start + duration : time(start + duration * x1);
        if (!(t1 > t0)) continue;
        const double v0 = gain * applyFadeCurve(curve, rising ? x0 : 1 - x0);
        const double v1 = gain * applyFadeCurve(curve, rising ? x1 : 1 - x1);
        env.ramp(t0, t1, float(v0), float(v1));
        t0 = t1;
    }
}
} // namespace

AudioPlan AudioPlanBuilder::build(const Project& project, const AudioSources& media, bool keepAllTracks) {
    AudioPlan plan;
    plan.duration = project.duration();
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) { uint64_t b; std::memcpy(&b, &v, 8); h ^= b; h *= 1099511628211ull; };
    for (const auto& track : project.audioTracks) {
        const bool audible = keepAllTracks || project.isAudible(track);
        if (!audible) continue;
        AudioLane lane;
        lane.track = track.id;
        const double trackGain = dbToLinear(track.gainDB);
        // Ducking under another track: the level follows that track's sound.
        std::optional<std::vector<std::pair<double, double>>> duckSpans;
        if (track.duckUnder) {
            const Track* v = project.track(*track.duckUnder);
            if (v && project.isAudible(*v)) duckSpans = AudioLevels::activity(*track.duckUnder, project, media);
        }
        const double duckLow = dbToLinear(track.duckDB);
        std::vector<Clip> clips;
        for (const auto& c : project.clips) if (c.trackID == track.id && c.kind == ClipKind::audio) clips.push_back(c);
        std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        double cursor = 0;
        for (Clip clip : clips) {
            const auto range = clip.speedRange();
            if (!(range.second <= 32) || !(range.first >= 1.0 / 32) || !clip.assetID || !media.sound) continue;
            auto snd = media.sound(*clip.assetID);
            if (!snd) continue;
            AudioVoice voice;
            voice.clip = clip.id;
            voice.asset = *clip.assetID;
            voice.path = snd->first;
            double rs = snd->second.first, re = snd->second.second;
            if (clip.playsBackwards()) {
                // The reversed sound runs from the end of the file's sound; line it up with the file's
                // length, which a reversed clip's source time is measured from.
                auto r = media.reversed ? media.reversed(*clip.assetID) : std::nullopt;
                const MediaAsset* a = project.asset(clip.assetID);
                if (!r || !a) { mix(-1); continue; }
                clip.sourceStart += r->second - a->duration;
                if (clip.ramp) clip.ramp = clip.ramp->shifted(r->second - a->duration);
                voice.path = r->first;
                voice.reversed = true;
                rs = 0;
                re = r->second;
                mix(-2);
            }
            if (!insert(clip, rs, re, voice, cursor)) continue;
            for (const auto& p : voice.pieces) { mix(p.t0); mix(p.t1); mix(p.s0); mix(p.s1); }
            const double gain = float(trackGain * dbToLinear(clip.gainDB));
            if (duckSpans && !duckSpans->empty()) {
                // Fades and ducking together, as short linear pieces every 50ms.
                const double step = 0.05;
                double t0 = clip.start;
                auto level = [&](double t) { return float(gain * clip.fadeGain(t) * AudioLevels::duckGain(t, *duckSpans, duckLow)); };
                while (t0 < clip.end() - 1e-6) {
                    const double t1 = std::min(clip.end(), t0 + step);
                    lane.volume.ramp(time(t0), time(t1), level(t0), level(t1));
                    t0 = t1;
                }
            } else {
                const double start = time(clip.start), end = time(clip.end());
                const double fi = std::min(time(clip.fadeIn), end - start);
                const double fo = std::min(time(clip.fadeOut), end - start - fi);
                if (fi > 0) fadeRamp(lane.volume, start, fi, gain, clip.inCurve(), true);
                else lane.volume.set(start, gain);
                if (fo > 0) fadeRamp(lane.volume, end - fo, fo, gain, clip.outCurve(), false);
            }
            lane.voices.push_back(std::move(voice));
        }
        if (lane.voices.empty()) continue;
        lane.volume.finish();
        plan.lanes.push_back(std::move(lane));
    }
    plan.signature = h;
    return plan;
}

} // namespace er
