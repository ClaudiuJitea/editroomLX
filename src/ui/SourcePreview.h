// SourcePreview.swift (SourcePreviewState): a library item, or the frame at a trimmed edge, shown in the main
// viewer over the edit, labelled with its name and time ("In"/"Out" while trimming). Frames are decoded one
// after another on a worker (a moment behind beats nothing): the latest asked-for time is fetched when the
// one being decoded is done.
#pragma once
#include "media/MediaStore.h"
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace er {

class VideoDecoder;

class SourcePreview : public QObject {
    Q_OBJECT
public:
    struct Current {
        MediaAsset asset;
        double time = 0;
        QImage image;
        /// Said after the name: "In" or "Out" while a clip's edge is being trimmed.
        QString note;
    };
    explicit SourcePreview(MediaStore& media, QObject* parent = nullptr);
    /// Waits for a frame being decoded (its worker uses this object's decoders).
    ~SourcePreview() override;
    const std::optional<Current>& current() const { return current_; }
    /// Hover-scrubbing a Media item, or (with a note) an edge being trimmed: its frame at `time` (file seconds).
    void show(const MediaAsset& asset, double time, const QString& note = {});
    void clear();
    /// A frame already at hand for a video (its made-ahead scrub frame), shown at once while the exact one is
    /// decoded.
    std::function<std::optional<QImage>(const Uuid& asset, double time)> quick;
signals:
    void changed();
private:
    void fetch();
    MediaStore& media_;
    std::optional<Current> current_;
    std::optional<double> wanted_;
    bool fetching_ = false;
    std::mutex decodersLock_;
    std::map<Uuid, std::shared_ptr<VideoDecoder>> decoders_;
    QThreadPool pool_;
};

} // namespace er
