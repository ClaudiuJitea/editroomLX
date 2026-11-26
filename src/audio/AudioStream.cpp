#include "AudioStream.h"
#include <QString>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>

namespace er {

namespace {
constexpr int kRate = 48000;
/// How far ahead of what's heard the sound is queued: enough to ride out a busy moment, little enough
/// that a change is heard soon after it's made.
constexpr double kAhead = 0.2;
constexpr int kBlock = 1024;

struct Block {
    double time = 0;             // timeline time of the first frame
    int frames = 0;
    std::vector<std::vector<float>> lanes; // interleaved stereo per lane
    std::vector<Uuid> laneTracks;
    bool end = false;            // the timeline's end (nothing after)
    bool wrap = false;           // the first block after a loop went round
};
} // namespace

struct AudioStream::Impl {
    ma_device device{};
    bool deviceOK = false;
    QString deviceName;
    Mixer mixer{kRate};
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Block> queue;
    size_t readPos = 0; // frames of queue.front() already played
    std::map<Uuid, float> laneVolume;
    std::atomic<bool> playing{false};
    std::atomic<double> heard{0};
    std::atomic<bool> endedFlag{false};
    std::atomic<bool> wrapped{false};
    double feedTime = 0;
    std::optional<std::pair<double, double>> loop;
    double duration = 0;
    bool stop = false;
    std::thread feeder;
    // Without a device: the clock runs on the system clock.
    std::chrono::steady_clock::time_point started;
    double startedAt = 0;
    int64_t consumedFrames = 0;
    std::vector<float> scratch;

    void callback(float* out, ma_uint32 frames) {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
        if (!playing) return;
        std::unique_lock g(lock, std::try_to_lock);
        if (!g.owns_lock()) return;
        consume(out, frames);
        g.unlock();
        wake.notify_all();
    }

    /// Plays `frames` from the queue into `out` (lock held).
    void consume(float* out, ma_uint32 frames) {
        ma_uint32 done = 0;
        while (done < frames && !queue.empty()) {
            Block& b = queue.front();
            if (b.wrap && readPos == 0) wrapped = true;
            const ma_uint32 n = std::min<ma_uint32>(frames - done, ma_uint32(b.frames - int(readPos)));
            for (size_t l = 0; l < b.lanes.size(); ++l) {
                auto it = laneVolume.find(b.laneTracks[l]);
                const float v = it == laneVolume.end() ? 1.0f : it->second;
                if (v == 0) continue;
                const float* src = b.lanes[l].data() + readPos * 2;
                for (ma_uint32 i = 0; i < n * 2; ++i) out[done * 2 + i] += src[i] * v;
            }
            heard = b.time + double(readPos + n) / kRate;
            readPos += n;
            done += n;
            if (readPos >= size_t(b.frames)) {
                if (b.end) endedFlag = true;
                queue.pop_front();
                readPos = 0;
            }
        }
    }

    static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
        static_cast<Impl*>(dev->pUserData)->callback(static_cast<float*>(out), frames);
    }

    double queuedSeconds() const {
        double s = 0;
        for (const auto& b : queue) s += double(b.frames) / kRate;
        return s - double(readPos) / kRate;
    }

    void feed() {
        std::unique_lock g(lock);
        while (!stop) {
            if (!deviceOK && playing) {
                const int64_t due = int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * kRate);
                if (due > consumedFrames) {
                    scratch.resize(size_t(due - consumedFrames) * 2);
                    consume(scratch.data(), ma_uint32(due - consumedFrames));
                    consumedFrames = due;
                }
            }
            if (!playing || queuedSeconds() >= kAhead || (feedTime >= duration && !loop)) {
                if (!deviceOK && playing) {
                    // No device: the queue is played out on the system clock, as the device would.
                    const int64_t due = int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * kRate);
                    if (due > consumedFrames) {
                        scratch.resize(size_t(due - consumedFrames) * 2);
                        consume(scratch.data(), ma_uint32(due - consumedFrames));
                        consumedFrames = due;
                    }
                }
                wake.wait_for(g, std::chrono::milliseconds(10));
                continue;
            }
            // One block, rendered outside the lock.
            double t = feedTime;
            bool wrap = false;
            if (loop && t >= loop->second - 1e-9) { t = loop->first; wrap = true; }
            int frames = kBlock;
            const double limit = loop ? loop->second : duration;
            const int left = int(std::ceil((limit - t) * kRate - 1e-6));
            frames = std::max(1, std::min(frames, left));
            Block b;
            b.time = t;
            b.frames = frames;
            b.wrap = wrap;
            b.end = !loop && t + double(frames) / kRate >= duration - 1e-9;
            g.unlock();
            const size_t lanes = mixer.plan().lanes.size();
            for (size_t l = 0; l < lanes; ++l) {
                std::vector<float> buf;
                mixer.renderLane(l, t, frames, buf);
                b.lanes.push_back(std::move(buf));
                b.laneTracks.push_back(mixer.plan().lanes[l].track);
            }
            g.lock();
            feedTime = t + double(frames) / kRate;
            queue.push_back(std::move(b));
        }
    }
};

AudioStream::AudioStream() : d(std::make_unique<Impl>()) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kRate;
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = d.get();
    cfg.periodSizeInMilliseconds = 10;
    if (qgetenv("EDITROOM_AUDIO") != "none" && ma_device_init(nullptr, &cfg, &d->device) == MA_SUCCESS) {
        d->deviceOK = ma_device_start(&d->device) == MA_SUCCESS;
        d->deviceName = QString::fromUtf8(d->device.playback.name);
    }
    d->feeder = std::thread([this] { d->feed(); });
}

AudioStream::~AudioStream() {
    {
        std::lock_guard g(d->lock);
        d->stop = true;
    }
    d->wake.notify_all();
    d->feeder.join();
    if (d->deviceOK) ma_device_uninit(&d->device);
}

bool AudioStream::hasDevice() const { return d->deviceOK; }
QString AudioStream::deviceName() const { return d->deviceName; }

void AudioStream::setPlan(const AudioPlan& plan) {
    std::lock_guard g(d->lock);
    d->mixer.setPlan(plan);
    d->duration = plan.duration;
    // What's queued was made from the old plan: drop it and read on from what's being heard, so an edit
    // is heard as soon as it's made, with no pause.
    if (d->playing) {
        d->queue.clear();
        d->readPos = 0;
        d->feedTime = d->heard;
    }
}

void AudioStream::setAudible(const std::map<Uuid, bool>& audible) {
    std::lock_guard g(d->lock);
    for (const auto& [id, on] : audible) d->laneVolume[id] = on ? 1.0f : 0.0f;
}

void AudioStream::setLoop(std::optional<std::pair<double, double>> span) {
    std::lock_guard g(d->lock);
    d->loop = span;
}

void AudioStream::play(double from) {
    {
        std::lock_guard g(d->lock);
        d->queue.clear();
        d->readPos = 0;
        d->feedTime = from;
        d->heard = from;
        d->endedFlag = false;
        d->started = std::chrono::steady_clock::now();
        d->startedAt = from;
        d->consumedFrames = 0;
        d->playing = true;
    }
    d->wake.notify_all();
}

void AudioStream::pause() {
    std::lock_guard g(d->lock);
    d->playing = false;
    d->queue.clear();
    d->readPos = 0;
}

void AudioStream::seek(double t) {
    const bool was = d->playing;
    pause();
    d->heard = t;
    if (was) play(t);
}

bool AudioStream::isPlaying() const { return d->playing; }
double AudioStream::currentTime() const { return d->heard; }
bool AudioStream::ended() const { return d->endedFlag; }
bool AudioStream::takeWrap() { return d->wrapped.exchange(false); }
double AudioStream::outputLatency() const {
    if (!d->deviceOK) return 0;
    return double(d->device.playback.internalPeriodSizeInFrames * d->device.playback.internalPeriods) / kRate;
}

} // namespace er
