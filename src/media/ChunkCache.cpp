#include "ChunkCache.h"
#include <QDateTime>
#include <QThreadPool>
#include <cmath>
#include <set>

namespace er {

namespace {
std::mutex registryLock;
std::set<ChunkCache*> registry;
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
} // namespace

std::shared_ptr<ChunkCache> ChunkCache::create(const QString& path) {
    std::shared_ptr<ChunkCache> c(new ChunkCache(path));
    std::lock_guard g(registryLock);
    registry.insert(c.get());
    return c;
}

ChunkCache::ChunkCache(const QString& path) : path_(path) {
    if (auto p = Probe::file(path); p && p->fps > 1) fps_ = p->fps;
}

ChunkCache::~ChunkCache() {
    {
        std::lock_guard g(registryLock);
        registry.erase(this);
    }
}

double ChunkCache::chunkLength(int limit) const {
    // A stretch is a set number of frames (fewer when they're big), so a 60 or 120 fps file doesn't hold
    // several times the memory of a 24 fps one.
    return limit > 2000 ? std::min(0.25, std::max(0.1, 8 / fps_)) : std::min(0.5, std::max(0.15, 16 / fps_));
}

void ChunkCache::releaseIfIdle(qint64 now) {
    std::lock_guard g(lock_);
    if (now - lastTouched_ > 2000) {
        for (auto it = chunks_.begin(); it != chunks_.end();) it = it->second->done ? chunks_.erase(it) : std::next(it);
    }
}

void ChunkCache::releaseAll() {
    std::lock_guard g(lock_);
    for (auto it = chunks_.begin(); it != chunks_.end();) it = it->second->done ? chunks_.erase(it) : std::next(it);
}

QString ChunkCache::decodePath() const {
    std::lock_guard g(lock_);
    return decodePath_;
}

std::shared_ptr<ChunkCache::Chunk> ChunkCache::chunk(int k, int limit, double length) {
    const Key key{k, limit};
    std::shared_ptr<Chunk> c;
    {
        std::lock_guard g(lock_);
        ++clock_;
        lastTouched_ = nowMs();
        if (auto it = chunks_.find(key); it != chunks_.end()) {
            it->second->lastUsed = clock_;
            return it->second;
        }
        c = std::make_shared<Chunk>();
        c->lastUsed = clock_;
        chunks_[key] = c;
        // Keep the three most recently used stretches: the one showing and those either side.
        while (chunks_.size() > 3) {
            auto oldest = chunks_.end();
            for (auto it = chunks_.begin(); it != chunks_.end(); ++it)
                if (!(it->first.index == k && it->first.limit == limit) && (oldest == chunks_.end() || it->second->lastUsed < oldest->second->lastUsed)) oldest = it;
            if (oldest == chunks_.end()) break;
            chunks_.erase(oldest);
        }
    }
    std::vector<ChunkCache*> others;
    {
        std::lock_guard g(registryLock);
        for (auto* r : registry) if (r != this) others.push_back(r);
    }
    const qint64 now = nowMs();
    for (auto* o : others) o->releaseIfIdle(now);
    auto self = shared_from_this();
    const double start = double(k) * length, end = double(k + 1) * length;
    QThreadPool::globalInstance()->start([self, c, start, end] { self->decode(c, start, end); });
    return c;
}

void ChunkCache::decode(const std::shared_ptr<Chunk>& c, double start, double end) {
    std::vector<VideoFrame> frames;
    {
        std::lock_guard d(decodeLock_);
        if (!decoder_) {
            decoder_ = VideoDecoder::open(path_);
            decodedTo_ = -1;
        }
        if (decoder_) {
            // Carry on when this stretch follows what was decoded last; otherwise seek.
            const bool follows = decodedTo_ >= 0 && start >= decodedTo_ - 1e-6 && start - decodedTo_ < 1.0;
            if (!follows) { decoder_->seek(start); lookahead_.reset(); }
            std::optional<VideoFrame> before;
            auto nextFrame = [&]() -> std::optional<VideoFrame> {
                if (lookahead_) { auto f = lookahead_; lookahead_.reset(); return f; }
                return decoder_->next();
            };
            decodedTo_ = -1;
            while (auto f = nextFrame()) {
                if (f->time >= end - 1e-6) {
                    // The first frame of the next stretch: kept for it.
                    if (frames.empty() && before) frames.push_back(*before);
                    if (frames.empty()) frames.push_back(*f);
                    lookahead_ = f;
                    decodedTo_ = end;
                    break;
                }
                if (f->time < start - 1e-6) { before = f; continue; }
                if (frames.empty() && before && f->time > start + 1e-6) frames.push_back(*before);
                frames.push_back(*f);
            }
            std::lock_guard g(lock_);
            decodePath_ = decoder_->path();
        }
    }
    std::lock_guard g(lock_);
    c->frames = std::move(frames);
    c->done = true;
    ready_.notify_all();
}

void ChunkCache::wait(const std::shared_ptr<Chunk>& c) {
    std::unique_lock g(lock_);
    ready_.wait_for(g, std::chrono::seconds(3), [&] { return c->done; });
}

std::optional<VideoFrame> ChunkCache::frameAt(double t, int limit) {
    const double len = chunkLength(limit);
    const int k = std::max(0, int(std::floor((t + 1e-6) / len)));
    auto c = chunk(k, limit, len);
    wait(c);
    // Playing forwards needs the stretch after this one next.
    chunk(k + 1, limit, len);
    {
        std::lock_guard g(lock_);
        for (auto it = c->frames.rbegin(); it != c->frames.rend(); ++it) if (it->time <= t + 1e-6) return *it;
        if (k == 0) return c->frames.empty() ? std::nullopt : std::optional<VideoFrame>(c->frames.front());
    }
    auto prev = chunk(k - 1, limit, len);
    wait(prev);
    std::lock_guard g(lock_);
    if (!prev->frames.empty()) return prev->frames.back();
    if (!c->frames.empty()) return c->frames.front();
    return std::nullopt;
}

std::optional<VideoFrame> ChunkCache::frameBefore(double t, int limit) {
    const double len = chunkLength(limit);
    const int k = std::max(0, int(std::floor((t - 1e-6) / len)));
    auto c = chunk(k, limit, len);
    wait(c);
    // Playing backwards needs the stretch before this one next.
    if (k > 0) chunk(k - 1, limit, len);
    {
        std::lock_guard g(lock_);
        for (auto it = c->frames.rbegin(); it != c->frames.rend(); ++it) if (it->time < t - 1e-6) return *it;
        if (k == 0) return c->frames.empty() ? std::nullopt : std::optional<VideoFrame>(c->frames.front());
    }
    auto prev = chunk(k - 1, limit, len);
    wait(prev);
    std::lock_guard g(lock_);
    if (!prev->frames.empty()) return prev->frames.back();
    if (!c->frames.empty()) return c->frames.front();
    return std::nullopt;
}

std::optional<VideoFrame> ChunkCache::frameAtIfReady(double t, int limit) {
    const double len = chunkLength(limit);
    const int k = std::max(0, int(std::floor((t + 1e-6) / len)));
    auto c = chunk(k, limit, len);
    std::lock_guard g(lock_);
    if (!c->done) return std::nullopt;
    for (auto it = c->frames.rbegin(); it != c->frames.rend(); ++it) if (it->time <= t + 1e-6) return *it;
    return c->frames.empty() ? std::nullopt : std::optional<VideoFrame>(c->frames.front());
}

} // namespace er
