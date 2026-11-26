#include "Mixer.h"
#include "media/Decoder.h"
#include <cmath>
#include <cstring>
#include <mutex>
#include <signalsmith-stretch.h>

namespace er {

namespace {

/// One clip's sound, read on from where the last call stopped.
struct VoiceState {
    const AudioVoice* voice = nullptr;
    std::unique_ptr<AudioDecoder> decoder;
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretch;
    int64_t nextFrame = -1;   // the timeline frame the next call should start at, to read on
    double inputDebt = 0;     // fractional input frames owed to the stretcher
    std::vector<float> in, inL, inR, outL, outR;
    bool failed = false;

    bool stretched() const {
        for (const auto& p : voice->pieces) if (std::abs(p.rate() - 1) > 1e-6) return true;
        return false;
    }

    const AudioPiece* pieceAt(double t) const {
        for (const auto& p : voice->pieces) if (t >= p.t0 - 1e-9 && t < p.t1) return &p;
        return nullptr;
    }

    double sourceAt(double t) const {
        const AudioPiece* p = pieceAt(t);
        if (!p) return voice->pieces.empty() ? 0 : voice->pieces.back().s1;
        return p->s0 + (t - p->t0) * p->rate();
    }

    void readInput(int frames, std::vector<float>& l, std::vector<float>& r) {
        in.clear();
        const int got = decoder->read(in, frames);
        l.assign(size_t(frames), 0.0f);
        r.assign(size_t(frames), 0.0f);
        for (int i = 0; i < got; ++i) { l[size_t(i)] = in[size_t(i) * 2]; r[size_t(i)] = in[size_t(i) * 2 + 1]; }
    }

    /// Starts reading at timeline time t (a seek, or the first call).
    void prime(double t, int rate) {
        if (!decoder) {
            decoder = AudioDecoder::open(voice->path, rate);
            if (!decoder) { failed = true; return; }
        }
        const double s = sourceAt(t);
        const AudioPiece* p = pieceAt(t);
        const double r = p ? p->rate() : 1.0;
        if (!stretched()) {
            decoder->seek(s);
            return;
        }
        if (!stretch) {
            stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>();
            stretch->presetDefault(2, float(rate));
        }
        stretch->reset();
        // Pre-roll from just before, so the first samples out line up with source time s.
        const int seekLen = stretch->outputSeekLength(float(r));
        decoder->seek(s);
        readInput(seekLen, inL, inR);
        float* chans[2] = {inL.data(), inR.data()};
        stretch->outputSeek(chans, seekLen);
        inputDebt = 0;
    }

    /// Adds `frames` of this voice's sound from timeline frame `f0` into `l`/`r` (unscaled).
    void render(int64_t f0, int frames, int rate, float* l, float* r) {
        if (failed) return;
        const double t0 = double(f0) / rate;
        if (nextFrame != f0) prime(t0, rate);
        if (failed) return;
        nextFrame = f0 + frames;
        if (!stretched()) {
            readInput(frames, inL, inR);
            for (int i = 0; i < frames; ++i) { l[i] += inL[size_t(i)]; r[i] += inR[size_t(i)]; }
            return;
        }
        // Each piece at its own rate, in blocks within the piece.
        int done = 0;
        while (done < frames) {
            const double t = double(f0 + done) / rate;
            const AudioPiece* p = pieceAt(t);
            int n = frames - done;
            if (p) n = std::max(1, std::min(n, int(std::ceil((p->t1 - t) * rate))));
            const double rateHere = p ? p->rate() : 1.0;
            inputDebt += n * rateHere;
            const int want = int(std::floor(inputDebt));
            inputDebt -= want;
            readInput(want, inL, inR);
            outL.assign(size_t(n), 0.0f);
            outR.assign(size_t(n), 0.0f);
            float* ins[2] = {inL.data(), inR.data()};
            float* outs[2] = {outL.data(), outR.data()};
            stretch->process(ins, want, outs, n);
            for (int i = 0; i < n; ++i) { l[done + i] += outL[size_t(i)]; r[done + i] += outR[size_t(i)]; }
            done += n;
        }
    }
};

} // namespace

struct Mixer::Impl {
    int rate;
    AudioPlan plan;
    std::map<Uuid, double> laneGains;
    std::vector<std::vector<std::unique_ptr<VoiceState>>> voices; // by lane
    std::mutex lock;

    void renderLane(size_t li, int64_t f0, int frames, float* l, float* r) {
        const AudioLane& lane = plan.lanes[li];
        std::vector<float> vl(static_cast<size_t>(frames)), vr(static_cast<size_t>(frames));
        const double t0 = double(f0) / rate, t1 = double(f0 + frames) / rate;
        for (size_t vi = 0; vi < lane.voices.size(); ++vi) {
            const AudioVoice& v = lane.voices[vi];
            if (v.end() <= t0 || v.start() >= t1) continue;
            // The voice's own part of this block.
            const int64_t a = std::max<int64_t>(f0, int64_t(std::ceil(v.start() * rate - 1e-6)));
            const int64_t b = std::min<int64_t>(f0 + frames, int64_t(std::ceil(v.end() * rate - 1e-6)));
            if (b <= a) continue;
            voices[li][vi]->render(a, int(b - a), rate, vl.data() + (a - f0), vr.data() + (a - f0));
        }
        // The lane's volume, sample by sample.
        for (int i = 0; i < frames; ++i) {
            const float g = float(lane.volume.at(double(f0 + i) / rate));
            l[i] += vl[size_t(i)] * g;
            r[i] += vr[size_t(i)] * g;
        }
    }
};

Mixer::Mixer(int rate) : d(std::make_unique<Impl>()) { d->rate = rate; }
Mixer::~Mixer() = default;

void Mixer::setPlan(const AudioPlan& plan) {
    std::lock_guard g(d->lock);
    // Voices that are still the same (same clip, file and pieces) keep their state, so a rebuild while
    // playing doesn't make every voice seek.
    // (Each voice's pieces are copied out first: its pointer into the old plan goes when the plan is replaced.)
    std::map<std::pair<Uuid, QString>, std::pair<std::unique_ptr<VoiceState>, std::vector<AudioPiece>>> old;
    for (size_t li = 0; li < d->voices.size(); ++li)
        for (auto& v : d->voices[li])
            if (v && v->voice) {
                auto key = std::make_pair(v->voice->clip, v->voice->path);
                auto pieces = v->voice->pieces;
                v->voice = nullptr;
                old[key] = {std::move(v), std::move(pieces)};
            }
    d->plan = plan;
    d->voices.clear();
    for (const auto& lane : d->plan.lanes) {
        std::vector<std::unique_ptr<VoiceState>> vs;
        for (const auto& v : lane.voices) {
            auto it = old.find({v.clip, v.path});
            std::unique_ptr<VoiceState> s;
            if (it != old.end()) {
                const auto& was = it->second.second;
                bool same = was.size() == v.pieces.size();
                for (size_t i = 0; same && i < v.pieces.size(); ++i) {
                    const auto& a = was[i];
                    same = a.t0 == v.pieces[i].t0 && a.t1 == v.pieces[i].t1 && a.s0 == v.pieces[i].s0 && a.s1 == v.pieces[i].s1;
                }
                if (same) s = std::move(it->second.first);
            }
            if (!s) s = std::make_unique<VoiceState>();
            s->voice = &v;
            vs.push_back(std::move(s));
        }
        d->voices.push_back(std::move(vs));
    }
}

const AudioPlan& Mixer::plan() const { return d->plan; }
int Mixer::rate() const { return d->rate; }

void Mixer::setLaneGains(const std::map<Uuid, double>& gains) {
    std::lock_guard g(d->lock);
    d->laneGains = gains;
}

void Mixer::render(double t0, int frames, std::vector<float>& out) {
    std::lock_guard g(d->lock);
    const size_t base = out.size();
    out.resize(base + size_t(frames) * 2, 0.0f);
    const int64_t f0 = int64_t(std::llround(t0 * d->rate));
    std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
    for (size_t li = 0; li < d->plan.lanes.size(); ++li) {
        auto it = d->laneGains.find(d->plan.lanes[li].track);
        const float gain = it == d->laneGains.end() ? 1.0f : float(it->second);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        d->renderLane(li, f0, frames, l.data(), r.data());
        for (int i = 0; i < frames; ++i) {
            out[base + size_t(i) * 2] += l[size_t(i)] * gain;
            out[base + size_t(i) * 2 + 1] += r[size_t(i)] * gain;
        }
    }
}

void Mixer::renderLane(size_t lane, double t0, int frames, std::vector<float>& out) {
    std::lock_guard g(d->lock);
    const size_t base = out.size();
    out.resize(base + size_t(frames) * 2, 0.0f);
    if (lane >= d->plan.lanes.size()) return;
    std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
    d->renderLane(lane, int64_t(std::llround(t0 * d->rate)), frames, l.data(), r.data());
    for (int i = 0; i < frames; ++i) { out[base + size_t(i) * 2] = l[size_t(i)]; out[base + size_t(i) * 2 + 1] = r[size_t(i)]; }
}

std::vector<float> Mixer::renderRange(double t0, double t1) {
    std::lock_guard g(d->lock);
    const int64_t f0 = int64_t(std::llround(t0 * d->rate));
    const int64_t total = std::max<int64_t>(0, int64_t(std::llround(t1 * d->rate)) - f0);
    const size_t lanes = d->plan.lanes.size();
    std::vector<std::vector<float>> L(lanes, std::vector<float>(size_t(total))), R(lanes, std::vector<float>(size_t(total)));
    // Lanes are independent: one thread each.
#pragma omp parallel for schedule(dynamic)
    for (long long li = 0; li < (long long)lanes; ++li) {
        const int block = d->rate; // a second at a time
        for (int64_t f = 0; f < total; f += block) {
            const int n = int(std::min<int64_t>(block, total - f));
            d->renderLane(size_t(li), f0 + f, n, L[size_t(li)].data() + f, R[size_t(li)].data() + f);
        }
    }
    std::vector<float> out(size_t(total) * 2, 0.0f);
    for (size_t li = 0; li < lanes; ++li) {
        auto it = d->laneGains.find(d->plan.lanes[li].track);
        const float gain = it == d->laneGains.end() ? 1.0f : float(it->second);
        for (int64_t i = 0; i < total; ++i) {
            out[size_t(i) * 2] += L[li][size_t(i)] * gain;
            out[size_t(i) * 2 + 1] += R[li][size_t(i)] * gain;
        }
    }
    return out;
}

} // namespace er
