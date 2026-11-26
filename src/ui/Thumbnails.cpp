#include "Thumbnails.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include "rendering/CpuRenderer.h"
#include <QThreadPool>
#include <cmath>
#include <cstring>

namespace er {

Thumbnails::Thumbnails(MediaStore& media, QObject* parent) : QObject(parent), media_(media) {}

Thumbnails::~Thumbnails() { pool_.waitForDone(); }

QImage Thumbnails::fit(const VideoFrame& frame, SourceColor color, int box) {
    const int dw = frame.displayWidth(), dh = frame.displayHeight();
    if (dw <= 0 || dh <= 0) return {};
    const double s = std::min(1.0, double(box) / std::max(dw, dh));
    const int w = std::max(2, int(std::round(dw * s))), h = std::max(2, int(std::round(dh * s)));
    FrameJob job;
    job.frame.canvasW = job.frame.outW = w;
    job.frame.canvasH = job.frame.outH = h;
    job.frame.layerCount = 1;
    cm::ErSource src{};
    src.format = frame.format; src.width = frame.width; src.height = frame.height;
    src.offset0 = frame.offset[0]; src.offset1 = frame.offset[1]; src.offset2 = frame.offset[2];
    src.stride0 = frame.stride[0]; src.stride1 = frame.stride[1];
    src.matrix = frame.matrix; src.fullRange = frame.fullRange; src.orientation = frame.orientation;
    job.sources = {src};
    job.pictures = {frame};
    cm::ErLayer L{};
    L.kind = ER_LAYER_PICTURE;
    L.gradeCube = -1;
    L.colorCube = -1;
    L.wipe = -1;
    L.scale = 1;
    L.opacity = 1;
    L.fitScale = float(std::min(double(w) / dw, double(h) / dh));
    if (auto cube = SourceColors::cube(color)) {
        L.colorCube = 0;
        job.cubes = *cube;
    }
    job.layers = {L};
    const auto px = CpuRenderer::render(job);
    QImage img(w, h, QImage::Format_RGBX8888);
    for (int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), px.data() + size_t(y) * w * 4, size_t(w) * 4);
    return img;
}

QString Thumbnails::key(const MediaAsset& asset, double time, int box) {
    return asset.id.toString() + u'@' + QString::number(qint64(std::round(time * 30))) + u'/' + QString::number(box);
}

void Thumbnails::forget(const Uuid& asset) {
    const QString prefix = asset.toString() + u'@';
    for (auto it = cache_.begin(); it != cache_.end();) it = it.key().startsWith(prefix) ? cache_.erase(it) : std::next(it);
    queues_[asset].decoder.reset();
    emit ready();
}

QImage Thumbnails::cached(const MediaAsset& asset, double time, int box) const {
    auto it = cache_.find(key(asset, time, box));
    return it == cache_.end() ? QImage() : *it;
}

QImage Thumbnails::get(const MediaAsset& asset, double time, int box) {
    const QString k = key(asset, time, box);
    if (auto it = cache_.find(k); it != cache_.end()) return *it;
    if (pending_.count(k)) return {};
    pending_.insert(k);
    Queue& q = queues_[asset.id];
    const auto rt = media_.runtime(asset.id);
    q.waiting.push_back({k, time, box, media_.readPath(asset), asset.kind, rt ? rt->orientation : 1, rt ? rt->color : SourceColor::standard});
    if (!q.running) work(asset.id);
    return {};
}

/// Makes the next picture asked for of one file, on a worker thread, then the one after.
void Thumbnails::work(const Uuid& id) {
    Queue& q = queues_[id];
    if (q.waiting.empty()) { q.running = false; return; }
    q.running = true;
    // Newest first: what's on screen now matters more than what scrolled past.
    const Request r = q.waiting.back();
    q.waiting.pop_back();
    std::shared_ptr<VideoDecoder> decoder = q.decoder;
    pool_.start([this, id, r, decoder] {
        const QString& path = r.path;
        const MediaKind kind = r.kind;
        const int orientation = r.orientation;
        const SourceColor color = r.color;
        QImage img;
        std::shared_ptr<VideoDecoder> dec = decoder;
        if (kind == MediaKind::image) {
            if (auto s = Stills::load(path, r.box)) img = s->convertToFormat(QImage::Format_RGBX8888);
        } else if (kind == MediaKind::video) {
            if (!dec) dec = std::shared_ptr<VideoDecoder>(VideoDecoder::open(path).release());
            if (dec) {
                if (auto f = dec->frameAt(r.time)) {
                    f->orientation = orientation;
                    img = fit(*f, color, r.box);
                }
            }
        }
        QMetaObject::invokeMethod(this, [this, id, r, img, dec] {
            pending_.erase(r.key);
            if (cache_.size() > 2000) cache_.clear();
            cache_.insert(r.key, img);
            queues_[id].decoder = dec;
            emit ready();
            work(id);
        }, Qt::QueuedConnection);
    });
}

} // namespace er
