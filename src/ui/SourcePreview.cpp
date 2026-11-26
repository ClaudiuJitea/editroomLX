#include "SourcePreview.h"
#include "Thumbnails.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include <QCoreApplication>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace er {

SourcePreview::SourcePreview(MediaStore& media, QObject* parent) : QObject(parent), media_(media) {}
SourcePreview::~SourcePreview() { pool_.waitForDone(); }

void SourcePreview::show(const MediaAsset& asset, double time, const QString& note) {
    if (!current_ || current_->asset.id != asset.id) {
        current_ = Current{asset, time, QImage(), note};
    } else {
        current_->time = time;
        current_->note = note;
    }
    if (quick) if (auto img = quick(asset.id, time)) current_->image = *img;
    wanted_ = time;
    emit changed();
    fetch();
}

void SourcePreview::fetch() {
    if (fetching_ || !wanted_ || !current_) return;
    const double time = *wanted_;
    wanted_.reset();
    fetching_ = true;
    const MediaAsset asset = current_->asset;
    const QString path = media_.readPath(asset);
    const MediaRuntime* rt = media_.runtime(asset.id);
    const SourceColor color = rt ? rt->color : SourceColor::standard;
    const int orientation = rt ? rt->orientation : 1;
    QPointer<SourcePreview> self(this);
    (void)QtConcurrent::run(&pool_, [this, self, asset, path, color, orientation, time] {
        QImage img;
        if (asset.kind == MediaKind::image) {
            if (auto s = Stills::load(path, 1920)) img = *s;
        } else {
            std::shared_ptr<VideoDecoder> dec;
            {
                std::lock_guard<std::mutex> g(decodersLock_);
                auto& slot = decoders_[asset.id];
                if (!slot) slot = std::shared_ptr<VideoDecoder>(VideoDecoder::open(path).release());
                dec = slot;
            }
            if (dec) {
                if (auto f = dec->frameAt(time)) {
                    f->orientation = orientation;
                    img = Thumbnails::fit(*f, color, 1920);
                }
            }
        }
        QMetaObject::invokeMethod(qApp, [self, asset, img] {
            if (!self) return;
            self->fetching_ = false;
            if (!self->current_ || self->current_->asset.id != asset.id) return;
            if (!img.isNull()) {
                self->current_->image = img;
                emit self->changed();
            }
            self->fetch();
        }, Qt::QueuedConnection);
    });
}

void SourcePreview::clear() {
    wanted_.reset();
    if (!current_) return;
    current_.reset();
    emit changed();
}

} // namespace er
