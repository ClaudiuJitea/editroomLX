#include "MediaStore.h"
#include "Stills.h"
#include <QFileInfo>
#include <set>

namespace er {

std::optional<FileStamp> FileStamp::of(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists()) return std::nullopt;
    return FileStamp{fi.size(), fi.lastModified()};
}

MediaStore::MediaStore(QObject* parent) : QObject(parent) {}
MediaStore::~MediaStore() = default;

QString MediaStore::readPath(const MediaAsset& a) const {
    if (a.playablePath && QFileInfo::exists(*a.playablePath)) return *a.playablePath;
    return a.path;
}

std::optional<MediaAsset> MediaStore::importFile(const QString& path, const MediaAsset* old) {
    const QString name = old ? old->name : QFileInfo(path).completeBaseName();
    const Uuid id = old ? old->id : newUuid();
    if (Stills::isImage(path)) {
        auto sz = Stills::size(path);
        if (!sz || sz->first <= 0 || sz->second <= 0) return std::nullopt;
        if (old && old->kind != MediaKind::image) return std::nullopt;
        MediaAsset a;
        a.id = id; a.path = path; a.name = name; a.kind = MediaKind::image;
        a.duration = 0; a.width = sz->first; a.height = sz->second; a.fps = 0; a.hasAudio = false;
        forget(id);
        loadStill(a);
        return a;
    }
    // LX decodes MKV, WebM, MXF and the rest directly: no converted copy (the Mac's playablePath).
    auto probe = Probe::file(path);
    if (!probe) return std::nullopt;
    const MediaKind kind = probe->hasVideo ? MediaKind::video : MediaKind::audio;
    if (old && old->kind != kind) return std::nullopt;
    forget(id);
    MediaAsset a;
    a.id = id; a.path = path; a.name = name; a.kind = kind;
    a.duration = probe->duration; a.width = probe->width; a.height = probe->height; a.fps = probe->fps;
    a.hasAudio = probe->hasAudio;
    if (old) a.playablePath = old->playablePath && QFileInfo::exists(*old->playablePath) ? old->playablePath : std::nullopt;
    adopt(a, *probe);
    return a;
}

void MediaStore::adopt(const MediaAsset& a, const ProbeResult& p) {
    auto e = std::make_shared<Entry>();
    e->probe = p;
    e->path = readPath(a);
    e->runtime.orientation = p.orientation;
    e->runtime.color = p.color;
    e->runtime.length = p.duration;
    e->runtime.width = p.width;
    e->runtime.height = p.height;
    e->hasAudio = p.hasAudio;
    if (p.hasVideo) e->frames = ChunkCache::create(e->path);
    {
        std::lock_guard g(lock_);
        entries_[a.id] = e;
        if (auto s = FileStamp::of(a.path)) stamps_[a.id] = *s;
        opening_.erase(a.id);
    }
    emit visualChanged();
}

void MediaStore::loadStill(const MediaAsset& a) {
    {
        std::lock_guard g(lock_);
        if (auto s = FileStamp::of(a.path)) stamps_[a.id] = *s;
    }
    auto frame = Stills::frame(readPath(a));
    std::lock_guard g(lock_);
    opening_.erase(a.id);
    if (!frame) return;
    auto e = std::make_shared<Entry>();
    e->runtime.still = *frame;
    e->runtime.width = frame->width;
    e->runtime.height = frame->height;
    e->path = readPath(a);
    entries_[a.id] = e;
    QMetaObject::invokeMethod(this, [this] { emit stillLoaded(); emit visualChanged(); }, Qt::QueuedConnection);
}

void MediaStore::restore(const std::vector<MediaAsset>& assets) {
    expect(assets);
    for (const auto& a : assets) {
        {
            std::lock_guard g(lock_);
            if (entries_.count(a.id)) continue;
        }
        if (a.kind == MediaKind::image) { loadStill(a); continue; }
        const QString path = readPath(a);
        std::optional<ProbeResult> p = QFileInfo::exists(path) ? Probe::file(path) : std::nullopt;
        if (p) adopt(a, *p);
        std::lock_guard g(lock_);
        opening_.erase(a.id);
    }
    emit visualChanged();
}

void MediaStore::expect(const std::vector<MediaAsset>& assets) {
    std::lock_guard g(lock_);
    for (const auto& a : assets) if (!entries_.count(a.id)) opening_.insert(a.id);
}

void MediaStore::forget(const Uuid& id) {
    std::lock_guard g(lock_);
    entries_.erase(id);
    stamps_.erase(id);
}

bool MediaStore::isOffline(const MediaAsset& a) const {
    std::lock_guard g(lock_);
    return !entries_.count(a.id);
}

bool MediaStore::looksOffline(const MediaAsset& a) const {
    std::lock_guard g(lock_);
    return !entries_.count(a.id) && !opening_.count(a.id);
}

std::map<Uuid, FileStamp> MediaStore::changedOnDisk(const std::vector<MediaAsset>& assets) const {
    std::map<Uuid, FileStamp> changed;
    std::lock_guard g(lock_);
    for (const auto& a : assets) {
        auto was = stamps_.find(a.id);
        if (was == stamps_.end()) continue;
        auto now = FileStamp::of(a.path);
        if (now && !(*now == was->second)) changed[a.id] = *now;
    }
    return changed;
}

const MediaRuntime* MediaStore::runtime(const Uuid& asset) const {
    std::lock_guard g(lock_);
    auto it = entries_.find(asset);
    return it == entries_.end() ? nullptr : &it->second->runtime;
}

std::optional<ProbeResult> MediaStore::probe(const Uuid& asset) const {
    std::lock_guard g(lock_);
    auto it = entries_.find(asset);
    return it == entries_.end() ? std::nullopt : it->second->probe;
}

QString MediaStore::soundPath(const Uuid& asset) const {
    std::lock_guard g(lock_);
    auto it = entries_.find(asset);
    return it != entries_.end() && it->second->hasAudio ? it->second->path : QString();
}

std::optional<Transcript> MediaStore::transcript(const Uuid& asset) const {
    if (!transcripts_) return std::nullopt;
    const QString p = soundPath(asset);
    return p.isEmpty() ? std::nullopt : transcripts_(p);
}

QString MediaStore::decodePath(const Uuid& asset) const {
    std::shared_ptr<ChunkCache> c;
    {
        std::lock_guard g(lock_);
        auto it = entries_.find(asset);
        if (it != entries_.end()) c = it->second->frames;
    }
    return c ? c->decodePath() : QString();
}

std::optional<VideoFrame> MediaStore::frame(const PlanLayer& layer, double sourceTime, int limit, bool wait) {
    std::shared_ptr<ChunkCache> c;
    {
        std::lock_guard g(lock_);
        auto it = entries_.find(layer.asset);
        if (it != entries_.end()) c = it->second->frames;
    }
    if (!c) return std::nullopt;
    if (!wait) return c->frameAtIfReady(sourceTime, limit);
    return layer.reversed ? c->frameBefore(sourceTime, limit) : c->frameAt(sourceTime, limit);
}

FrameProvider MediaStore::provider(int limit, bool wait) {
    return [this, limit, wait](int, const PlanLayer& layer, double t) { return frame(layer, t, limit, wait); };
}

} // namespace er
