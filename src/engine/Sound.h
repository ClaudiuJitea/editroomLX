// The audio side's view of the media store: where each file's sound is, its reversed copy, its waveform.
// Waveforms are read once per file (in the background for the UI, at once for export) and cached on disk.
#pragma once
#include "audio/AudioPlan.h"
#include "audio/Waveform.h"
#include "media/MediaStore.h"
#include <QObject>
#include <mutex>

namespace er {

class SoundStore : public QObject {
    Q_OBJECT
public:
    explicit SoundStore(MediaStore& media, QObject* parent = nullptr);
    /// For AudioPlanBuilder. `wait`: read missing waveforms and reversed sound now (export).
    AudioSources sources(bool wait = false);
    const Waveform* waveform(const Uuid& asset, bool wait = false);
    /// Makes a file's reversed sound in the background (EditorModel.reverseSelection).
    void prepareReversed(const Uuid& asset);
    std::optional<std::pair<QString, double>> reversed(const Uuid& asset, bool wait = false);
    /// Starts reading the waveforms of these files in the background.
    void warm(const std::vector<MediaAsset>& assets);

signals:
    void waveformReady(const QUuid& asset);
    void reversedReady(const QUuid& asset);

private:
    MediaStore& media_;
    std::mutex lock_;
    std::map<Uuid, std::shared_ptr<Waveform>> waves_;
    std::set<Uuid> reading_, reversing_;
};

} // namespace er
