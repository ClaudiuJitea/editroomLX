// Exporter and ExportQueue (Exporter.swift). One export at a time in the background, each a snapshot of
// the project when it was added; the loudness target measures the mix first; reversed sound is made
// first; progress, cancel, and finished items tidying themselves away after 12 s.
//
// Each frame: the plan's FrameJob (decoded frames waited for), composited on the GPU (or the CPU) and
// packed to NV12 in the same submit, read back once and handed to the encoder.
#pragma once
#include "ExportSettings.h"
#include "Sound.h"
#include "rendering/GpuRenderer.h"
#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <thread>

namespace er {

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString encoder;
    QString renderer;
    QString decode;
    double seconds = 0;
    long long frames = 0;
};

/// Runs one export (blocking). `progress` gets 0…1; `cancel` is checked every frame.
ExportResult runExport(const Project& whole, MediaStore& media, SoundStore& sound, GpuRenderer* gpu,
                       const ExportSettings& settings, const QString& path,
                       const std::function<void(double)>& progress, const std::atomic<bool>& cancel);

class ExportQueue : public QObject {
    Q_OBJECT
public:
    enum class Status { waiting, running, done, failed, cancelled };
    struct Item {
        Uuid id = newUuid();
        QString path;
        Project project;
        ExportSettings settings;
        Status status = Status::waiting;
        QString message;
        QString name() const;
    };

    ExportQueue(MediaStore& media, SoundStore& sound, QObject* parent = nullptr);
    ~ExportQueue() override;

    void add(const Project& project, const ExportSettings& settings, const QString& path);
    void cancel(const Uuid& id);
    void clearFinished();
    const std::vector<Item>& items() const { return items_; }
    const Item* current() const;
    int waiting() const;
    bool isBusy() const;
    double progress() const { return progress_; }
    /// Seconds left on the current export, once there's enough progress to guess.
    std::optional<double> timeLeft() const;
    /// Set while the list is open, so finished items don't vanish from under the mouse.
    void setHoldOpen(bool hold);

signals:
    void changed();
    void finished(const QString& path, bool ok, const QString& message);

private:
    void startNext();
    void scheduleClear();
    MediaStore& media_;
    SoundStore& sound_;
    std::unique_ptr<GpuRenderer> gpu_;
    bool gpuTried_ = false;
    std::vector<Item> items_;
    double progress_ = 0;
    QDateTime startedAt_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    QTimer clearTimer_;
    bool holdOpen_ = false;
};

} // namespace er
