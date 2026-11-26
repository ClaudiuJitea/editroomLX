// Pictures of media for the Media panel tiles, filmstrips and hover previews (AVAssetImageGenerator and
// ColorFrameGrabber on the Mac): a decoded frame fitted into a box, through the same compositor kernel as
// playback, so HDR and Log frames are converted exactly as the timeline shows them.
#pragma once
#include "media/MediaStore.h"
#include <QHash>
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace er {

class VideoDecoder;

class Thumbnails : public QObject {
    Q_OBJECT
public:
    explicit Thumbnails(MediaStore& media, QObject* parent = nullptr);
    /// Waits for pictures being made: their workers report back to this object.
    ~Thumbnails() override;
    /// The picture of `asset` at `time` (file seconds), no bigger than `box`; null until it's made
    /// (then `ready` is emitted). Kept by time rounded to the frame.
    QImage get(const MediaAsset& asset, double time, int box = 480);
    /// The picture if it's already made; never asks for one (a stand-in while a neighbour is made).
    QImage cached(const MediaAsset& asset, double time, int box = 480) const;
    /// Lets go of a file's pictures (it changed on disk, or was relinked).
    void forget(const Uuid& asset);
    /// The Media tile's picture: a tenth of the way in, at most one second.
    QImage poster(const MediaAsset& asset) { return get(asset, std::min(1.0, asset.duration * 0.1), 240); }
    static QImage fit(const VideoFrame& frame, SourceColor color, int box);

signals:
    void ready();

private:
    static QString key(const MediaAsset& asset, double time, int box);
    void work(const Uuid& asset);
    MediaStore& media_;
    QHash<QString, QImage> cache_;
    std::set<QString> pending_;
    /// Each file's requests, worked through in order on one thread with one decoder kept open, so a filmstrip's
    /// frames (near each other in the file) don't each open the file and seek from scratch.
    struct Request { QString key; double time; int box; QString path; MediaKind kind; int orientation; SourceColor color; };
    struct Queue {
        std::deque<Request> waiting;
        bool running = false;
        std::shared_ptr<VideoDecoder> decoder;
    };
    std::map<Uuid, Queue> queues_;
    QThreadPool pool_;
};

} // namespace er
