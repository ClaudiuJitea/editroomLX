// ChunkCache: ReverseFrames.swift, ported, and used for every video clip (forwards, backwards, HDR), since
// LX has no AVComposition. Frames are decoded in short stretches ("chunks": 0.5 s, fewer frames when big),
// the three used last are kept, the next one in the playing direction is read ahead, and an idle cache
// lets its frames go after ~2 s when another starts decoding.
//
// Unlike the Mac (a fresh AVAssetReader per chunk), one decoder per file carries on from where it
// stopped when the next chunk follows the last, so playing forwards never seeks.
#pragma once
#include "Decoder.h"
#include <QString>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace er {

class ChunkCache : public std::enable_shared_from_this<ChunkCache> {
public:
    static std::shared_ptr<ChunkCache> create(const QString& path);
    ~ChunkCache();

    /// The frame showing at file time `t`, for playing forwards: the last one starting at or before it.
    std::optional<VideoFrame> frameAt(double t, int limit);
    /// The frame showing just before file time `t`, for playing backwards.
    std::optional<VideoFrame> frameBefore(double t, int limit);
    /// Without waiting: the frame if its chunk is decoded, else nothing (the chunk is started).
    std::optional<VideoFrame> frameAtIfReady(double t, int limit);
    void releaseAll();
    QString decodePath() const;
    double fps() const { return fps_; }

private:
    explicit ChunkCache(const QString& path);
    struct Chunk {
        std::vector<VideoFrame> frames;
        bool done = false;
        int lastUsed = 0;
    };
    struct Key {
        int index, limit;
        bool operator<(const Key& o) const { return std::tie(index, limit) < std::tie(o.index, o.limit); }
    };
    double chunkLength(int limit) const;
    std::shared_ptr<Chunk> chunk(int k, int limit, double length);
    void wait(const std::shared_ptr<Chunk>& c);
    void decode(const std::shared_ptr<Chunk>& c, double start, double end);
    void releaseIfIdle(qint64 now);

    QString path_;
    double fps_ = 30;
    mutable std::mutex lock_;
    std::condition_variable ready_;
    std::map<Key, std::shared_ptr<Chunk>> chunks_;
    int clock_ = 0;
    qint64 lastTouched_ = 0;
    std::mutex decodeLock_;
    std::unique_ptr<VideoDecoder> decoder_;
    double decodedTo_ = -1;
    std::optional<VideoFrame> lookahead_;
    QString decodePath_;
};

} // namespace er
