#include "ScrubCache.h"
#include "audio/Waveform.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include "rendering/CpuRenderer.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QStandardPaths>
#include <cmath>
#include <cstring>

namespace er {

namespace {

/// A decoded frame as Rec.709 RGB at its own orientation, no bigger than `maxSide`: the YUV-to-RGB step only
/// (standard video needs no colour conversion), as Core Image's unmanaged context gives the Mac's.
QImage toRGB(const VideoFrame& frame, int side = ScrubCache::maxSide) {
    const int fw = frame.width, fh = frame.height;
    if (fw <= 0 || fh <= 0) return {};
    const double s = std::min(1.0, double(side) / std::max(fw, fh));
    const int w = std::max(2, int(std::round(fw * s))), h = std::max(2, int(std::round(fh * s)));
    FrameJob job;
    job.frame.canvasW = job.frame.outW = w;
    job.frame.canvasH = job.frame.outH = h;
    job.frame.layerCount = 1;
    cm::ErSource src{};
    src.format = frame.format; src.width = frame.width; src.height = frame.height;
    src.offset0 = frame.offset[0]; src.offset1 = frame.offset[1]; src.offset2 = frame.offset[2];
    src.stride0 = frame.stride[0]; src.stride1 = frame.stride[1];
    src.matrix = frame.matrix; src.fullRange = frame.fullRange; src.orientation = 1;
    job.sources = {src};
    job.pictures = {frame};
    cm::ErLayer L{};
    L.kind = ER_LAYER_PICTURE;
    L.gradeCube = -1;
    L.colorCube = -1;
    L.wipe = -1;
    L.scale = 1;
    L.opacity = 1;
    L.fitScale = float(std::min(double(w) / fw, double(h) / fh));
    job.layers = {L};
    const auto px = CpuRenderer::render(job);
    QImage img(w, h, QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), px.data() + size_t(y) * w * 4, size_t(w) * 4);
    return img;
}

} // namespace

QImage ScrubCache::image(const VideoFrame& frame, int side) { return toRGB(frame, side); }

namespace {

QByteArray encode(const QImage& img) {
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    img.convertToFormat(QImage::Format_RGB888).save(&b, "JPEG", 90);
    return out;
}

} // namespace

ScrubCache::ScrubCache(QObject* parent) : QObject(parent) { pool_.setMaxThreadCount(1); }
ScrubCache::~ScrubCache() {
    {
        std::lock_guard g(lock_);
        queue_.clear();
    }
    pool_.waitForDone();
}

QString ScrubCache::folder() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/Editroom/Scrub");
    QDir().mkpath(d);
    return d;
}

bool ScrubCache::has(const Uuid& id) const {
    std::lock_guard g(lock_);
    return frames_.count(id) > 0;
}

std::optional<VideoFrame> ScrubCache::frame(const Uuid& id, double time) const {
    std::shared_ptr<Store> s;
    {
        std::lock_guard g(lock_);
        auto it = frames_.find(id);
        if (it == frames_.end()) return std::nullopt;
        s = it->second;
    }
    if (s->ranges.empty()) return std::nullopt;
    const int i = std::min(int(s->ranges.size()) - 1, std::max(0, int(std::round(time * rate))));
    {
        std::lock_guard g(lock_);
        if (auto it = last_.find(id); it != last_.end() && it->second.first == i) return it->second.second;
    }
    const auto [a, b] = s->ranges[size_t(i)];
    QImage img;
    if (!img.loadFromData(s->data + a, int(b - a), "JPEG")) return std::nullopt;
    VideoFrame f = Stills::toFrame(img.convertToFormat(QImage::Format_RGBA8888));
    std::lock_guard g(lock_);
    last_[id] = {i, f};
    return f;
}

void ScrubCache::make(const Uuid& id, const QString& path, double duration) {
    {
        std::lock_guard g(lock_);
        if (working_ == id && stale_.count(id)) { again_[id] = {path, duration}; return; }
        if (frames_.count(id) || working_ == id || !(duration > 0)) return;
        for (const auto& q : queue_) if (std::get<0>(q) == id) return;
        queue_.emplace_back(id, path, duration);
    }
    next();
}

void ScrubCache::forget(const Uuid& id) {
    std::lock_guard g(lock_);
    frames_.erase(id);
    last_.erase(id);
    std::erase_if(queue_, [&](const auto& q) { return std::get<0>(q) == id; });
    if (working_ == id) stale_.insert(id);
}

void ScrubCache::next() {
    Uuid id;
    QString path;
    double duration;
    {
        std::lock_guard g(lock_);
        if (working_ || queue_.empty()) return;
        std::tie(id, path, duration) = queue_.front();
        queue_.pop_front();
        working_ = id;
    }
    QPointer<ScrubCache> self(this);
    pool_.start([this, self, id, path, duration] {
        auto made = build(path, duration);
        QMetaObject::invokeMethod(this, [this, id, made] {
            bool done = false;
            {
                std::lock_guard g(lock_);
                working_.reset();
                if (stale_.erase(id)) {
                    if (auto it = again_.find(id); it != again_.end()) { queue_.emplace_back(id, it->second.first, it->second.second); again_.erase(it); }
                } else if (made) {
                    frames_[id] = made;
                    done = true;
                }
            }
            if (done) emit this->made(id);
            next();
        }, Qt::QueuedConnection);
    });
}

void ScrubCache::waitForDone() {
    for (;;) {
        {
            std::lock_guard g(lock_);
            if (!working_ && queue_.empty()) return;
        }
        pool_.waitForDone(10);
        QCoreApplication::processEvents();
    }
}

/// Reads the file through once, start to end (much faster than seeking), keeping a frame every twelfth of a second.
std::shared_ptr<ScrubCache::Store> ScrubCache::build(const QString& source, double duration) {
    const QString file = folder() + u'/' + WaveformReader::cacheName(source) + QStringLiteral(".scrub709lx");
    if (auto saved = load(file)) return saved;
    auto dec = VideoDecoder::open(source, VideoDecoder::Accel::software);
    if (!dec) return nullptr;
    const int count = int(std::ceil(duration * rate)) + 1;
    // Written to disk as they're made: first room for how many and where each ends, then the frames; the ends go
    // in once they're all known.
    const QString partial = file + QStringLiteral(".part");
    QFile out(partial);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return nullptr;
    const qint64 head = 8 + qint64(count) * 8;
    out.write(QByteArray(int(head), '\0'));
    std::vector<quint64> ends;
    quint64 size = 0;
    QByteArray last;
    auto put = [&](const QByteArray& f) {
        out.write(f);
        size += quint64(f.size());
        ends.push_back(size);
        last = f;
    };
    while (int(ends.size()) < count) {
        auto f = dec->next();
        if (!f) break;
        const double t = f->time;
        // This frame stands for every twelfth of a second it's the latest one for.
        QByteArray encoded;
        while (int(ends.size()) < count && double(ends.size()) / rate <= t + 0.5 / rate) {
            if (encoded.isEmpty()) encoded = encode(toRGB(*f));
            if (encoded.isEmpty()) { out.close(); QFile::remove(partial); return nullptr; }
            put(encoded);
        }
    }
    if (last.isEmpty()) { out.close(); QFile::remove(partial); return nullptr; }
    // The end of the file: the last frame stands in.
    while (int(ends.size()) < count) put(last);
    out.seek(0);
    const quint64 n = quint64(count);
    out.write(reinterpret_cast<const char*>(&n), 8);
    for (quint64 e : ends) out.write(reinterpret_cast<const char*>(&e), 8);
    out.close();
    QFile::remove(file);
    if (!QFile::rename(partial, file)) return nullptr;
    trim();
    return load(file);
}

/// Keeps the folder under 6 GB: the files used longest ago go first (they're made again if needed).
void ScrubCache::trim() {
    struct F { QString path; QDateTime used; qint64 size; };
    std::vector<F> files;
    qint64 total = 0;
    for (const QFileInfo& fi : QDir(folder()).entryInfoList({QStringLiteral("*.scrub709lx")}, QDir::Files)) {
        files.push_back({fi.absoluteFilePath(), fi.lastRead(), fi.size()});
        total += fi.size();
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.used < b.used; });
    for (const auto& f : files) {
        if (total <= 6'000'000'000LL) break;
        QFile::remove(f.path);
        total -= f.size;
    }
}

/// A saved file, mapped rather than read, with its frames' places.
std::shared_ptr<ScrubCache::Store> ScrubCache::load(const QString& path) {
    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::ReadOnly) || file->size() <= 8) return nullptr;
    const uchar* data = file->map(0, file->size());
    if (!data) return nullptr;
    auto word = [&](qint64 at) { quint64 v; std::memcpy(&v, data + at, 8); return qint64(v); };
    const qint64 n = word(0);
    const qint64 head = 8 + n * 8;
    if (n <= 0 || file->size() < head) return nullptr;
    auto s = std::make_shared<Store>();
    s->file = file;
    s->data = data;
    s->size = file->size();
    qint64 start = head;
    for (qint64 i = 0; i < n; ++i) {
        const qint64 end = head + word(8 + i * 8);
        if (end <= start || end > s->size) return nullptr;
        s->ranges.emplace_back(start, end);
        start = end;
    }
    return s;
}

} // namespace er
