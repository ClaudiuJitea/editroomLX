#include "AudioStream.h"
#include <QString>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <vector>

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

/// How much later than it's handed over the output plays sound: a few tens of ms for the speakers, around
/// 0.15–0.3 s for Bluetooth headphones, which take the sound that long to send (AirPods: about 171 ms). Read from
/// the sound server (PipeWire, or PulseAudio) as a stream's `sink_usec` ("the time a sample takes to be played on
/// the sink"), on a short silent probe stream through libpulse (LGPL, loaded when it's there; nothing is linked).
/// 0 when it can't be read.
double measureSinkLatency() {
    if (const QByteArray forced = qgetenv("EDITROOM_SINK_LATENCY"); !forced.isEmpty()) return forced.toDouble();
    static void* lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
    auto sym = [](const char* n) { return dlsym(lib, n); };
    using P = void*;
    struct SampleSpec { int format; uint32_t rate; uint8_t channels; };
    static auto loopNew = reinterpret_cast<P (*)()>(sym("pa_threaded_mainloop_new"));
    static auto loopApi = reinterpret_cast<P (*)(P)>(sym("pa_threaded_mainloop_get_api"));
    static auto loopStart = reinterpret_cast<int (*)(P)>(sym("pa_threaded_mainloop_start"));
    static auto loopStop = reinterpret_cast<void (*)(P)>(sym("pa_threaded_mainloop_stop"));
    static auto loopFree = reinterpret_cast<void (*)(P)>(sym("pa_threaded_mainloop_free"));
    static auto loopLock = reinterpret_cast<void (*)(P)>(sym("pa_threaded_mainloop_lock"));
    static auto loopUnlock = reinterpret_cast<void (*)(P)>(sym("pa_threaded_mainloop_unlock"));
    static auto ctxNew = reinterpret_cast<P (*)(P, const char*)>(sym("pa_context_new"));
    static auto ctxConnect = reinterpret_cast<int (*)(P, const char*, int, P)>(sym("pa_context_connect"));
    static auto ctxState = reinterpret_cast<int (*)(P)>(sym("pa_context_get_state"));
    static auto ctxDisconnect = reinterpret_cast<void (*)(P)>(sym("pa_context_disconnect"));
    static auto ctxUnref = reinterpret_cast<void (*)(P)>(sym("pa_context_unref"));
    static auto streamNew = reinterpret_cast<P (*)(P, const char*, const SampleSpec*, P)>(sym("pa_stream_new"));
    static auto streamConnect = reinterpret_cast<int (*)(P, const char*, P, int, P, P)>(sym("pa_stream_connect_playback"));
    static auto streamState = reinterpret_cast<int (*)(P)>(sym("pa_stream_get_state"));
    static auto streamWrite = reinterpret_cast<int (*)(P, const void*, size_t, P, int64_t, int)>(sym("pa_stream_write"));
    static auto streamTiming = reinterpret_cast<const uint8_t* (*)(P)>(sym("pa_stream_get_timing_info"));
    static auto streamDisconnect = reinterpret_cast<int (*)(P)>(sym("pa_stream_disconnect"));
    static auto streamUnref = reinterpret_cast<void (*)(P)>(sym("pa_stream_unref"));
    if (!loopNew || !loopApi || !loopStart || !loopStop || !loopFree || !loopLock || !loopUnlock || !ctxNew || !ctxConnect || !ctxState ||
        !ctxDisconnect || !ctxUnref || !streamNew || !streamConnect || !streamState || !streamWrite || !streamTiming || !streamDisconnect || !streamUnref)
        return 0;
    P loop = loopNew();
    if (!loop) return 0;
    P ctx = ctxNew(loopApi(loop), "Editroom");
    double result = 0;
    P stream = nullptr;
    // Polled for a state, at most a second: true once it's `ready`, false if it failed.
    auto waitFor = [&](auto state, P obj, int ready, int failed, int terminated) {
        for (int i = 0; i < 100; ++i) {
            loopLock(loop);
            const int st = state(obj);
            loopUnlock(loop);
            if (st == ready) return true;
            if (st == failed || st == terminated) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    if (ctx && ctxConnect(ctx, nullptr, 0, nullptr) >= 0 && loopStart(loop) >= 0 && waitFor(ctxState, ctx, 4 /* READY */, 5, 6)) {
        const SampleSpec spec{5 /* FLOAT32LE */, uint32_t(kRate), 2};
        loopLock(loop);
        stream = streamNew(ctx, "latency probe", &spec, nullptr);
        // Interpolated and automatically updated timing, so `sink_usec` is filled in.
        if (stream) streamConnect(stream, nullptr, nullptr, 0x2 | 0x8, nullptr, nullptr);
        loopUnlock(loop);
        if (stream && waitFor(streamState, stream, 2 /* READY */, 3, 4)) {
            // A tenth of a second of silence, so the stream runs and the server times the output.
            const std::vector<float> silence(size_t(kRate / 10) * 2, 0.0f);
            loopLock(loop);
            streamWrite(stream, silence.data(), silence.size() * sizeof(float), nullptr, 0, 0);
            loopUnlock(loop);
            // Read for a little while and keep the largest: the first readings come before the output has timed it.
            for (int i = 0; i < 6; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                loopLock(loop);
                // pa_timing_info: struct timeval timestamp (16 bytes), int synchronized_clocks (+4 padding), then
                // pa_usec_t sink_usec at byte 24 (unchanged since PulseAudio 0.9).
                if (const uint8_t* ti = streamTiming(stream)) {
                    uint64_t usec = 0;
                    std::memcpy(&usec, ti + 24, sizeof usec);
                    result = std::max(result, double(usec) / 1e6);
                }
                loopUnlock(loop);
            }
        }
    }
    loopLock(loop);
    if (stream) { streamDisconnect(stream); streamUnref(stream); }
    if (ctx) { ctxDisconnect(ctx); ctxUnref(ctx); }
    loopUnlock(loop);
    loopStop(loop);
    loopFree(loop);
    return std::clamp(result, 0.0, 1.0);
}

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
    /// The end of the sound handed to the device so far (timeline time).
    std::atomic<double> heard{0};
    /// Where the sound last started from (play, seek, a loop going round): what's heard is never before it.
    std::atomic<double> anchor{0};
    /// The output's own delay (Bluetooth headphones), from the sound server.
    std::atomic<double> sinkLatency{0};
    /// When everything up to the end was handed over (steady clock, seconds), so the rest plays out.
    std::atomic<double> endedAt{-1};
    std::thread prober;
    std::condition_variable probeWake;
    std::mutex probeLock;
    bool probeNow = true;
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
            if (b.wrap && readPos == 0) { wrapped = true; anchor = b.time; }
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
                if (b.end) {
                    endedAt = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    endedFlag = true;
                }
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
    // The output's delay: read at the start, when playback starts, and every few seconds while playing (headphones
    // connected or taken off midway). Only through the sound server, which is where miniaudio plays.
    const bool server = d->deviceOK && d->device.pContext && d->device.pContext->backend == ma_backend_pulseaudio;
    if (server || qEnvironmentVariableIsSet("EDITROOM_SINK_LATENCY")) {
        d->prober = std::thread([this] {
            std::unique_lock g(d->probeLock);
            while (!d->stop) {
                d->probeWake.wait_for(g, std::chrono::seconds(4), [this] { return d->stop || d->probeNow || d->playing; });
                if (d->stop) break;
                const bool asked = d->probeNow;
                d->probeNow = false;
                if (!asked && !d->playing) continue;
                g.unlock();
                d->sinkLatency = measureSinkLatency();
                g.lock();
                if (!asked) d->probeWake.wait_for(g, std::chrono::seconds(4), [this] { return d->stop || d->probeNow; });
            }
        });
    }
}

AudioStream::~AudioStream() {
    {
        std::lock_guard g(d->lock);
        d->stop = true;
    }
    d->wake.notify_all();
    {
        std::lock_guard p(d->probeLock);
        d->probeWake.notify_all();
    }
    d->feeder.join();
    if (d->prober.joinable()) d->prober.join();
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
        d->anchor = from;
        d->endedAt = -1;
        d->endedFlag = false;
        d->started = std::chrono::steady_clock::now();
        d->startedAt = from;
        d->consumedFrames = 0;
        d->playing = true;
    }
    d->wake.notify_all();
    {
        // The output may have changed since (headphones on): read its delay again.
        std::lock_guard p(d->probeLock);
        d->probeNow = true;
    }
    d->probeWake.notify_all();
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
    d->anchor = t;
    if (was) play(t);
}

bool AudioStream::isPlaying() const { return d->playing; }
double AudioStream::currentTime() const {
    // What's heard now: what was handed over, less what's still on its way (the device's buffer and the output's
    // own delay), never before where the sound last started.
    if (!d->playing) return d->heard;
    const double lag = outputLatency();
    double t = d->heard - lag;
    // The end handed over: the rest plays out on the clock.
    if (const double e = d->endedAt; e >= 0) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        t = std::min(double(d->heard), t + (now - e));
    }
    return std::max(double(d->anchor), t);
}
bool AudioStream::ended() const { return d->endedFlag; }
bool AudioStream::takeWrap() { return d->wrapped.exchange(false); }
double AudioStream::outputLatency() const {
    const double sink = d->sinkLatency;
    if (!d->deviceOK) return sink;
    return double(d->device.playback.internalPeriodSizeInFrames * d->device.playback.internalPeriods) / kRate + sink;
}

double AudioStream::sinkLatency() const { return d->sinkLatency; }

} // namespace er
