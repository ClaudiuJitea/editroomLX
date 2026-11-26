#include "AudioScrubber.h"
#include "Mixer.h"
#include <QByteArray>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <miniaudio.h>

namespace er {

struct AudioScrubber::Impl {
    ma_device device{};
    bool deviceOK = false, running = false;
    // Shared with the device callback, under `lock`.
    mutable std::mutex lock;
    std::vector<int16_t> samples; // interleaved stereo
    int readFrame = 0, remaining = 0, fadeIn = 0;
    std::optional<size_t> fingerprint;
    std::atomic<int> generation{0};
    std::atomic<bool> ready{false};
    std::thread worker;

    static void callback(ma_device* dev, void* out, const void*, ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        float* o = static_cast<float*>(out);
        std::lock_guard<std::mutex> g(self->lock);
        int pos = self->readFrame, remain = self->remaining, fade = self->fadeIn;
        const int count = int(self->samples.size() / 2);
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            if (remain > 0 && pos < count && pos >= 0) {
                // Ease in after each jump and out at the end, so nothing clicks.
                float gain = 1;
                if (fade < 96) { gain = float(fade) / 96; ++fade; }
                if (remain < 160) gain *= float(remain) / 160;
                o[i * 2] = float(self->samples[size_t(pos) * 2]) / 32768 * gain;
                o[i * 2 + 1] = float(self->samples[size_t(pos) * 2 + 1]) / 32768 * gain;
                ++pos;
                --remain;
            } else {
                o[i * 2] = o[i * 2 + 1] = 0;
            }
        }
        self->readFrame = pos;
        self->remaining = remain;
        self->fadeIn = fade;
    }
};

AudioScrubber::AudioScrubber() : d(std::make_unique<Impl>()) {
    if (qgetenv("EDITROOM_AUDIO") == "none") return;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = ma_uint32(rate);
    cfg.dataCallback = &Impl::callback;
    cfg.pUserData = d.get();
    // A short buffer: scrubbing should answer within a few milliseconds.
    cfg.periodSizeInMilliseconds = 10;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    d->deviceOK = ma_device_init(nullptr, &cfg, &d->device) == MA_SUCCESS;
}

AudioScrubber::~AudioScrubber() {
    ++d->generation;
    if (d->worker.joinable()) d->worker.join();
    if (d->deviceOK) ma_device_uninit(&d->device);
}

void AudioScrubber::warmUp() {
    if (d->deviceOK && !d->running) d->running = ma_device_start(&d->device) == MA_SUCCESS;
}

void AudioScrubber::shutDown() {
    {
        std::lock_guard<std::mutex> g(d->lock);
        d->remaining = 0;
    }
    if (d->deviceOK && d->running) { ma_device_stop(&d->device); d->running = false; }
}

void AudioScrubber::prepare(const Project& project, const AudioSources& media, size_t fingerprint) {
    if (d->fingerprint == fingerprint) return;
    d->fingerprint = fingerprint;
    const int gen = ++d->generation;
    if (d->worker.joinable()) d->worker.join();
    const double duration = project.duration();
    AudioPlan plan = AudioPlanBuilder::build(project, media);
    if (!(duration > 0) || duration >= maxSeconds || plan.lanes.empty()) {
        std::lock_guard<std::mutex> g(d->lock);
        d->samples.clear();
        d->remaining = 0;
        d->ready = false;
        return;
    }
    d->ready = false;
    d->worker = std::thread([this, plan = std::move(plan), duration, gen] {
        Mixer mixer(static_cast<int>(rate));
        mixer.setPlan(plan);
        std::vector<int16_t> out;
        out.reserve(size_t(duration * rate * 2) + 4096);
        // In one-second pieces, so a newer edit can cut a long render short.
        std::vector<float> chunk;
        for (double t = 0; t < duration; t += 1) {
            if (d->generation != gen) return;
            chunk.clear();
            mixer.render(t, int(std::min(1.0, duration - t) * rate + 0.5), chunk);
            for (float v : chunk) out.push_back(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767)));
        }
        if (d->generation != gen) return;
        std::lock_guard<std::mutex> g(d->lock);
        d->samples = std::move(out);
        d->ready = true;
    });
}

void AudioScrubber::play(double time) {
    warmUp();
    std::lock_guard<std::mutex> g(d->lock);
    d->readFrame = int(std::max(0.0, time) * rate);
    d->remaining = int(snippet * rate);
    d->fadeIn = 0;
}

bool AudioScrubber::ready() const { return d->ready; }

void AudioScrubber::waitReady(int ms) const {
    for (int i = 0; i < ms / 10 && !d->ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

float AudioScrubber::peak(double time, double seconds) const {
    std::lock_guard<std::mutex> g(d->lock);
    const long a = long(time * rate) * 2, b = std::min(long(d->samples.size()), a + long(seconds * rate) * 2);
    if (a < 0 || a >= b) return 0;
    int m = 0;
    for (long i = a; i < b; ++i) m = std::max(m, std::abs(int(d->samples[size_t(i)])));
    return float(m) / 32768;
}

} // namespace er
