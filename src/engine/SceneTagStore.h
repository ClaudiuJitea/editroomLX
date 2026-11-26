// SceneTagStore (SceneTags.swift): what each clip and photo shows, made once per file in the background and kept
// on disk. The Mac sees frames with Apple's Vision classifier (and Apple Intelligence's keywords); here OpenAI's
// CLIP (ViT-B/32, MIT) runs on this computer through ONNX Runtime and picks, for each frame, the labels in
// Vision's own words that fit it best. Nothing is sent anywhere. The model (about 150 MB) is downloaded once,
// only when the person asks for it (Settings › General).
#pragma once
#include "model/SceneTags.h"
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

class QNetworkAccessManager;
class QNetworkReply;

namespace er {

class SceneLooker;

class SceneTagStore : public QObject {
    Q_OBJECT
public:
    explicit SceneTagStore(QObject* parent = nullptr);
    ~SceneTagStore() override;

    /// ONNX Runtime is installed (libonnxruntime).
    static bool runtimeAvailable();
    /// The model is on this computer.
    static bool available();
    static QString modelFolder();
    /// Its three files: the picture and text halves, and the text half's word list.
    static QStringList modelFiles();
    static QString modelURL(const QString& file);
    static constexpr qint64 modelBytes = 89117001 + 64504507 + 2224119;
    void downloadModel(std::function<void(double)> progress, std::function<void(QString)> done);
    void cancelDownload();
    bool downloading() const { return reply_ != nullptr; }

    std::optional<SceneTags> tags(const QString& path);
    /// Queues a file: a video (`isVideo`, with its length) or a photo.
    void make(const QString& path, bool isVideo, double duration);
    /// A file changed on disk: what was seen in it goes.
    void forget(const QString& path);
    bool pending(const QString& path) const;
    void waitForDone();

    /// Kept: labels the looker is fairly sure of somewhere in the clip (of every label, for one frame).
    static constexpr double sure = 0.08;
    /// Looks at a video, or a photo, now, on this thread.
    static std::optional<SceneTags> seeVideo(const QString& path, double duration);
    static std::optional<SceneTags> seePhoto(const QString& path);
    /// Every label for one picture, surest first (tests, and tuning).
    static std::vector<std::pair<QString, double>> classify(const QImage& image);
    /// MediaAnalyzer's aesthetics (VNCalculateImageAestheticsScoresRequest): a score from −1 (badly made) to 1
    /// (well made), and whether it's a screenshot or a document rather than a photo. None without the model.
    struct Aesthetics { double score; bool utility; };
    static std::optional<Aesthetics> aesthetics(const QImage& image);
    /// The labels the looker chooses from.
    static const QStringList& labels();
    static QString folder();

signals:
    void changed();
    void modelInstalled();

private:
    struct Job { QString path; bool isVideo; double duration; };
    QString file(const QString& path) const;
    std::optional<SceneTags> load(const QString& path);
    void save(const SceneTags& t, const QString& path);
    void next();
    void fetch(int index, std::function<void(double)> progress, std::function<void(QString)> done);
    mutable std::mutex lock_;
    std::map<QString, SceneTags> made_;
    std::deque<Job> queue_;
    std::optional<QString> working_;
    std::set<QString> stale_;
    std::map<QString, Job> again_;
    QThreadPool pool_;
    QNetworkAccessManager* net_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    qint64 fetched_ = 0;
};

} // namespace er
