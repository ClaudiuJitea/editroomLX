// ScrubCache.swift: frames from each video, twelve a second, made ahead in the background so dragging the
// playhead or skimming shows a picture at once (exact seeks into long-GOP footage take tens of milliseconds
// each, so the viewer fell behind a quick drag). As sharp as the viewer's own picture (up to 1920 pixels), one
// file per video on disk (laid out as the Mac's: a count, where each frame ends, then the frames), mapped and
// read only as frames are needed, so long footage doesn't fill memory; reopening a project has them at once.
// The file's own pixels, not turned upright: the compositor turns them as it does the player's. Only for
// standard (Rec.709) video, as on the Mac. LX stores JPEG frames where the Mac stores HEIC (docs/PARITY.md).
#pragma once
#include "rendering/VideoFrame.h"
#include "model/Types.h"
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

class QFile;

namespace er {

class ScrubCache : public QObject {
    Q_OBJECT
public:
    static constexpr double rate = 12.0;
    /// The longest side: the preview's own size, so a scrub frame is as sharp as the exact one.
    static constexpr int maxSide = 1920;
    /// A decoded frame as RGB, no bigger than `side` on its long side (scene tags look at these too).
    static QImage image(const VideoFrame& frame, int side = maxSide);

    explicit ScrubCache(QObject* parent = nullptr);
    ~ScrubCache() override;
    bool has(const Uuid& id) const;
    /// The frame nearest a time in the file, if its frames are made (decoded here, any thread).
    std::optional<VideoFrame> frame(const Uuid& id, double time) const;
    /// Queues a video's frames (once).
    void make(const Uuid& id, const QString& path, double duration);
    void forget(const Uuid& id);
    /// The folder the frames are kept in (XDG cache).
    static QString folder();
    /// Waits for the frames being made (tests).
    void waitForDone();
signals:
    void made(const QUuid& id);

private:
    struct Store {
        std::shared_ptr<QFile> file;
        const uchar* data = nullptr;
        qint64 size = 0;
        std::vector<std::pair<qint64, qint64>> ranges;
    };
    static std::shared_ptr<Store> load(const QString& path);
    static std::shared_ptr<Store> build(const QString& source, double duration);
    static void trim();
    void next();
    mutable std::mutex lock_;
    std::map<Uuid, std::shared_ptr<Store>> frames_;
    std::deque<std::tuple<Uuid, QString, double>> queue_;
    std::optional<Uuid> working_;
    std::set<Uuid> stale_;
    std::map<Uuid, std::pair<QString, double>> again_;
    /// The frame decoded last for each video, so a scrub resting on one place doesn't decode it again.
    mutable std::map<Uuid, std::pair<int, VideoFrame>> last_;
    QThreadPool pool_;
};

} // namespace er
