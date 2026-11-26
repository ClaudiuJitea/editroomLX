// MediaStore.swift: the files a project uses, opened. Import (probe), stills, the frame caches every
// video layer is drawn from, "changed on disk" by size and date, and "offline" only after loading has
// finished. It is the renderer's MediaInfo.
#pragma once
#include "ChunkCache.h"
#include "Decoder.h"
#include "rendering/RenderPlan.h"
#include <QDateTime>
#include <QObject>
#include <mutex>

namespace er {

struct FileStamp {
    qint64 size = 0;
    QDateTime date;
    static std::optional<FileStamp> of(const QString& path);
    bool operator==(const FileStamp& o) const { return size == o.size && date == o.date; }
};

class MediaStore : public QObject, public MediaInfo {
    Q_OBJECT
public:
    explicit MediaStore(QObject* parent = nullptr);
    ~MediaStore() override;

    /// Reads a file and describes it as a library item (blocking: call off the main thread for big
    /// files). `replacing` keeps an existing item's id and name and refuses a different kind.
    std::optional<MediaAsset> importFile(const QString& path, const MediaAsset* replacing = nullptr);
    /// Opens the files of a project that was just loaded (blocking).
    void restore(const std::vector<MediaAsset>& assets);
    void expect(const std::vector<MediaAsset>& assets);
    void forget(const Uuid& id);

    bool isOffline(const MediaAsset& a) const;
    bool looksOffline(const MediaAsset& a) const;
    std::map<Uuid, FileStamp> changedOnDisk(const std::vector<MediaAsset>& assets) const;

    // MediaInfo
    const MediaRuntime* runtime(const Uuid& asset) const override;
    std::optional<Transcript> transcript(const Uuid& asset) const override;
    void setTranscripts(std::function<std::optional<Transcript>(const QString& path)> f) { transcripts_ = std::move(f); }

    /// The frame for a plan's video layer at a file time (ChunkCache), no bigger than `limit`.
    /// `wait`: block until it's decoded (export, exact seeks); otherwise only what's ready.
    std::optional<VideoFrame> frame(const PlanLayer& layer, double sourceTime, int limit, bool wait = true);
    /// A FrameProvider for describeFrame.
    FrameProvider provider(int limit, bool wait = true);

    /// The path a library item reads from (its converted copy when the Mac made one and it's here).
    QString readPath(const MediaAsset& a) const;
    QString decodePath(const Uuid& asset) const;
    QString soundPath(const Uuid& asset) const;
    std::optional<ProbeResult> probe(const Uuid& asset) const;

signals:
    void visualChanged();
    void stillLoaded();

private:
    void adopt(const MediaAsset& a, const ProbeResult& probe);
    void loadStill(const MediaAsset& a);
    struct Entry {
        MediaRuntime runtime;
        std::optional<ProbeResult> probe;
        std::shared_ptr<ChunkCache> frames;
        QString path;
        bool hasAudio = false;
    };
    mutable std::mutex lock_;
    std::map<Uuid, std::shared_ptr<Entry>> entries_;
    std::map<Uuid, FileStamp> stamps_;
    std::set<Uuid> opening_;
    std::function<std::optional<Transcript>(const QString&)> transcripts_;
};

} // namespace er
