#include "Sound.h"
#include "audio/ReverseAudio.h"
#include <QThreadPool>

namespace er {

SoundStore::SoundStore(MediaStore& media, QObject* parent) : QObject(parent), media_(media) {}

AudioSources SoundStore::sources(bool wait) {
    AudioSources s;
    s.sound = [this](const Uuid& id) -> std::optional<std::pair<QString, std::pair<double, double>>> {
        auto p = media_.probe(id);
        if (!p || !p->hasAudio) return std::nullopt;
        return std::make_pair(media_.soundPath(id), std::make_pair(0.0, p->audioEnd));
    };
    s.reversed = [this, wait](const Uuid& id) { return reversed(id, wait); };
    s.peaks = [this, wait](const Uuid& id, double* pps) -> const std::vector<float>* {
        const Waveform* w = waveform(id, wait);
        if (!w) return nullptr;
        *pps = w->peaksPerSecond;
        return &w->peaks;
    };
    return s;
}

const Waveform* SoundStore::waveform(const Uuid& asset, bool wait) {
    {
        std::lock_guard g(lock_);
        if (auto it = waves_.find(asset); it != waves_.end()) return it->second.get();
        if (!wait && reading_.count(asset)) return nullptr;
    }
    const QString path = media_.soundPath(asset);
    if (path.isEmpty()) return nullptr;
    if (!wait) {
        {
            std::lock_guard g(lock_);
            reading_.insert(asset);
        }
        QThreadPool::globalInstance()->start([this, asset, path] {
            auto wf = WaveformReader::forFile(path);
            {
                std::lock_guard g(lock_);
                reading_.erase(asset);
                if (wf) waves_[asset] = std::make_shared<Waveform>(std::move(*wf));
            }
            if (wf) QMetaObject::invokeMethod(this, [this, asset] { emit waveformReady(asset); }, Qt::QueuedConnection);
        });
        return nullptr;
    }
    auto wf = WaveformReader::forFile(path);
    if (!wf) return nullptr;
    std::lock_guard g(lock_);
    auto& slot = waves_[asset];
    slot = std::make_shared<Waveform>(std::move(*wf));
    return slot.get();
}

std::optional<std::pair<QString, double>> SoundStore::reversed(const Uuid& asset, bool wait) {
    const QString path = media_.soundPath(asset);
    if (path.isEmpty()) return std::nullopt;
    if (auto e = ReverseAudio::existing(path)) return e;
    if (wait) return ReverseAudio::render(path);
    prepareReversed(asset);
    return std::nullopt;
}

void SoundStore::prepareReversed(const Uuid& asset) {
    const QString path = media_.soundPath(asset);
    if (path.isEmpty() || ReverseAudio::existing(path)) return;
    {
        std::lock_guard g(lock_);
        if (!reversing_.insert(asset).second) return;
    }
    QThreadPool::globalInstance()->start([this, asset, path] {
        ReverseAudio::render(path);
        {
            std::lock_guard g(lock_);
            reversing_.erase(asset);
        }
        QMetaObject::invokeMethod(this, [this, asset] { emit reversedReady(asset); }, Qt::QueuedConnection);
    });
}

void SoundStore::warm(const std::vector<MediaAsset>& assets) {
    for (const auto& a : assets) if (a.kind != MediaKind::image && a.hasAudio) waveform(a.id, false);
}

} // namespace er
