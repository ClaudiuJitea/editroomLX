#include "SceneTagStore.h"
#include "ScrubCache.h"
#include "audio/Waveform.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include "model/Json.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include "Onnx.h"
#include <cmath>

namespace er {

// MARK: The labels

const QStringList& SceneTagStore::labels() {
    // Vision's own words: every label the everyday words point at (SceneTags.aliases), and the other common ones
    // its classifier gives for home video.
    static const QStringList all = [] {
        QStringList l;
        for (const auto& [k, v] : SceneTags::aliases()) l << k;
        for (const char* w : {
                 "animal", "bird", "fish", "insect", "butterfly", "flower", "tree", "grass", "field", "farm", "desert",
                 "canyon", "cliff", "cave", "island", "coast", "shore", "harbor", "pier", "bridge", "tunnel", "road",
                 "highway", "parking lot", "sidewalk", "crosswalk", "traffic", "night", "sunset sunrise", "rainbow",
                 "rain", "fog", "storm", "lightning", "moon", "sun", "clouds", "office", "classroom", "kitchen",
                 "living room", "bedroom", "bathroom", "hallway", "stairs", "window", "furniture", "sofa", "table",
                 "chair", "bed", "television", "computer", "phone", "camera", "book", "paper", "document", "text",
                 "sign", "poster", "screen", "whiteboard", "map", "clothing", "shoes", "hat", "glasses", "jewelry",
                 "dress", "suit", "uniform", "costume", "toy", "ball", "balloon", "gift", "candle", "christmas tree",
                 "pumpkin", "fruit", "vegetable", "bread", "pizza", "burger", "sushi", "ice cream", "chocolate",
                 "dessert", "salad", "soup", "pasta", "meat", "cheese", "tea", "juice", "water", "bottle", "cup",
                 "plate", "bowl", "knife", "shop", "market", "supermarket", "mall", "hotel", "church", "temple",
                 "mosque", "tower", "lighthouse", "windmill", "barn", "fence", "wall", "graffiti", "sculpture",
                 "fountain", "pool", "gym", "workout", "running", "walking", "jumping", "climbing", "fishing",
                 "gardening", "cooking", "eating", "reading", "writing", "typing", "talking", "singing", "playing",
                 "hugging", "kissing", "crying", "sleeping", "portrait", "face", "hand", "group photo", "family",
                 "couple", "dancer", "performer", "stage", "audience", "lecture", "meeting", "interview", "podcast",
                 "conference", "tool", "machine", "engine", "construction", "factory", "warehouse", "robot",
                 "drone", "sky", "space", "star", "fireplace", "smoke", "ice", "frost", "autumn", "spring",
                 "summer", "leaves", "rock", "sand", "wave", "underwater", "boardwalk", "pet", "rabbit", "cow",
                 "sheep", "pig", "chicken", "duck", "goat", "deer", "bear", "lion", "tiger", "elephant", "giraffe",
                 "monkey", "dolphin", "whale", "shark", "turtle", "frog", "snake", "spider"}) {
            const QString s = QLatin1String(w);
            if (!l.contains(s)) l << s;
        }
        return l;
    }();
    return all;
}

// MARK: CLIP

namespace {

/// CLIP's tokenizer for the labels' prompts: lower-case words, byte-level BPE with its merges (labels are plain
/// ASCII, which byte-level coding leaves as it is).
class Tokenizer {
public:
    bool load(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QJsonObject model = QJsonDocument::fromJson(f.readAll()).object().value("model").toObject();
        const QJsonObject vocab = model.value("vocab").toObject();
        for (auto it = vocab.begin(); it != vocab.end(); ++it) vocab_[it.key()] = it.value().toInt();
        const QJsonArray merges = model.value("merges").toArray();
        for (int i = 0; i < merges.size(); ++i) {
            const QJsonValue m = merges[i];
            const QString pair = m.isArray() ? m.toArray()[0].toString() + u' ' + m.toArray()[1].toString() : m.toString();
            ranks_[pair] = i;
        }
        return !vocab_.empty() && !ranks_.empty();
    }
    std::vector<int64_t> encode(const QString& text) const {
        std::vector<int64_t> ids{49406};
        static const QRegularExpression words(QStringLiteral("[\\p{L}]+|[\\p{N}]|[^\\s\\p{L}\\p{N}]+"));
        auto it = words.globalMatch(text.toLower());
        while (it.hasNext()) {
            const QString w = it.next().captured();
            std::vector<QString> parts;
            for (int i = 0; i < w.size(); ++i) parts.push_back(QString(w[i]));
            parts.back() += QStringLiteral("</w>");
            for (;;) {
                int best = -1, rank = INT_MAX;
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    auto r = ranks_.find(parts[i] + u' ' + parts[i + 1]);
                    if (r != ranks_.end() && r->second < rank) { rank = r->second; best = int(i); }
                }
                if (best < 0) break;
                parts[best] += parts[best + 1];
                parts.erase(parts.begin() + best + 1);
            }
            for (const auto& p : parts) if (auto v = vocab_.find(p); v != vocab_.end()) ids.push_back(v->second);
        }
        ids.push_back(49407);
        return ids;
    }
private:
    std::map<QString, int> vocab_;
    std::map<QString, int> ranks_;
};

void normalize(std::vector<float>& v) {
    double n = 0;
    for (float x : v) n += double(x) * x;
    n = std::sqrt(std::max(n, 1e-12));
    for (float& x : v) x = float(x / n);
}

QString modelFile(const char* name) { return SceneTagStore::modelFolder() + u'/' + QLatin1String(name); }

} // namespace

/// CLIP, loaded once and kept (the store's worker is the only user), with the labels' prompts already seen.
class SceneLooker {
public:
    static SceneLooker* shared() {
        static std::mutex m;
        std::lock_guard g(m);
        static std::unique_ptr<SceneLooker> one;
        static QString loaded;
        if (one && loaded == SceneTagStore::modelFolder()) return one.get();
        one.reset();
        if (!onnx::load()) return nullptr;
        loaded = SceneTagStore::modelFolder();
        try {
            auto l = std::make_unique<SceneLooker>();
            if (l->ready()) one = std::move(l);
        } catch (const Ort::Exception&) {}
        return one.get();
    }

    SceneLooker() : env_(onnx::env()) {
        Ort::SessionOptions o;
        o.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount() - 1));
        o.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        vision_ = std::make_unique<Ort::Session>(env_, modelFile("vision_model_quantized.onnx").toLocal8Bit().constData(), o);
        text_ = std::make_unique<Ort::Session>(env_, modelFile("text_model_quantized.onnx").toLocal8Bit().constData(), o);
        Tokenizer tok;
        if (!tok.load(modelFile("tokenizer.json"))) return;
        for (const QString& t : promptTexts()) prompts_.push_back(embedText(tok, t));
        labelEmbeddings(tok);
    }

    bool ready() const { return !labels_.empty(); }

    /// Every label's share of the picture (CLIP's softmax over all of them, at its own temperature).
    std::vector<std::pair<QString, double>> classify(const QImage& image) {
        std::vector<float> e = embedImage(image);
        if (e.empty()) return {};
        const auto& names = SceneTagStore::labels();
        std::vector<double> logits(names.size());
        double top = -1e9;
        for (int i = 0; i < names.size(); ++i) {
            double d = 0;
            for (size_t k = 0; k < e.size(); ++k) d += double(e[k]) * labels_[i][k];
            logits[i] = 100.0 * d;
            top = std::max(top, logits[i]);
        }
        double sum = 0;
        for (double& l : logits) { l = std::exp(l - top); sum += l; }
        std::vector<std::pair<QString, double>> out;
        for (int i = 0; i < names.size(); ++i) out.push_back({names[i], logits[i] / sum});
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return out;
    }

    /// Shares of some prompts of its own, for the aesthetics.
    std::vector<double> compare(const QImage& image, const std::vector<int>& which) {
        std::vector<float> e = embedImage(image);
        if (e.empty()) return {};
        std::vector<double> l;
        double top = -1e9;
        for (int w : which) {
            double d = 0;
            for (size_t k = 0; k < e.size(); ++k) d += double(e[k]) * prompts_[w][k];
            l.push_back(100.0 * d);
            top = std::max(top, l.back());
        }
        double sum = 0;
        for (double& x : l) { x = std::exp(x - top); sum += x; }
        for (double& x : l) x /= sum;
        return l;
    }
    /// The aesthetics prompts: 0 well made, 1 badly made; 2 a photo, 3 a screenshot, 4 a document.
    static const std::vector<QString>& promptTexts() {
        static const std::vector<QString> p = {
            QStringLiteral("a beautiful, well composed, sharp photo."), QStringLiteral("a blurry, dull, badly composed photo."),
            QStringLiteral("a photo."), QStringLiteral("a screenshot of a computer screen."), QStringLiteral("a scan of a text document.")};
        return p;
    }

private:
    static std::string outputNamed(Ort::Session& s, const char* want) {
        Ort::AllocatorWithDefaultOptions a;
        const size_t n = s.GetOutputCount();
        std::string first;
        for (size_t i = 0; i < n; ++i) {
            std::string name = s.GetOutputNameAllocated(i, a).get();
            if (i == 0) first = name;
            if (name == want) return name;
        }
        return first;
    }

    std::vector<float> run(Ort::Session& s, const char* want, std::vector<std::string> inNames, std::vector<Ort::Value>& inputs) {
        const std::string out = outputNamed(s, want);
        std::vector<const char*> in;
        for (auto& n : inNames) in.push_back(n.c_str());
        const char* outName = out.c_str();
        auto r = s.Run(Ort::RunOptions{nullptr}, in.data(), inputs.data(), inputs.size(), &outName, 1);
        const auto info = r[0].GetTensorTypeAndShapeInfo();
        const float* d = r[0].GetTensorData<float>();
        std::vector<float> v(d, d + info.GetElementCount());
        normalize(v);
        return v;
    }

    void labelEmbeddings(const Tokenizer& tok) {
        const auto& names = SceneTagStore::labels();
        // Seen once and kept beside the model (named for the labels, so a new list is seen again).
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(names.join(u'|').toUtf8());
        QFile cache(modelFile("labels-") + QString::fromLatin1(h.result().toHex().left(12)) + QStringLiteral(".bin"));
        if (cache.open(QIODevice::ReadOnly)) {
            const QByteArray b = cache.readAll();
            const qsizetype dim = names.isEmpty() ? 0 : b.size() / qsizetype(sizeof(float)) / names.size();
            if (dim > 0 && b.size() == dim * names.size() * qsizetype(sizeof(float))) {
                const float* f = reinterpret_cast<const float*>(b.constData());
                for (int i = 0; i < names.size(); ++i) labels_.emplace_back(f + i * dim, f + (i + 1) * dim);
                return;
            }
            cache.close();
        }
        for (const QString& n : names) labels_.push_back(embedText(tok, QStringLiteral("a photo of %1.").arg(n)));
        QSaveFile out(cache.fileName());
        if (out.open(QIODevice::WriteOnly)) {
            for (const auto& v : labels_) out.write(reinterpret_cast<const char*>(v.data()), qint64(v.size() * sizeof(float)));
            out.commit();
        }
    }

    std::vector<float> embedText(const Tokenizer& tok, const QString& text) {
        Ort::AllocatorWithDefaultOptions a;
        std::vector<std::string> inNames;
        for (size_t i = 0; i < text_->GetInputCount(); ++i) inNames.push_back(text_->GetInputNameAllocated(i, a).get());
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> ids = tok.encode(text);
        std::vector<int64_t> mask(ids.size(), 1);
        const std::array<int64_t, 2> shape{1, int64_t(ids.size())};
        std::vector<Ort::Value> in;
        for (const auto& name : inNames)
            in.push_back(Ort::Value::CreateTensor<int64_t>(mem, name == "attention_mask" ? mask.data() : ids.data(), ids.size(), shape.data(), 2));
        return run(*text_, "text_embeds", inNames, in);
    }

    /// CLIP's preprocessing: shortest side to 224 (bicubic), the middle 224 × 224, scaled and normalised.
    std::vector<float> embedImage(const QImage& src) {
        if (src.isNull()) return {};
        const double s = 224.0 / std::min(src.width(), src.height());
        const QImage scaled = src.convertToFormat(QImage::Format_RGB888)
                                  .scaled(std::max(224, int(std::round(src.width() * s))), std::max(224, int(std::round(src.height() * s))),
                                          Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage crop = scaled.copy((scaled.width() - 224) / 2, (scaled.height() - 224) / 2, 224, 224);
        static const float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f}, stdev[3] = {0.26862954f, 0.26130258f, 0.27577711f};
        std::vector<float> px(3 * 224 * 224);
        for (int y = 0; y < 224; ++y) {
            const uchar* row = crop.constScanLine(y);
            for (int x = 0; x < 224; ++x)
                for (int c = 0; c < 3; ++c) px[size_t(c) * 224 * 224 + y * 224 + x] = (row[x * 3 + c] / 255.0f - mean[c]) / stdev[c];
        }
        Ort::AllocatorWithDefaultOptions a;
        std::vector<std::string> inNames{vision_->GetInputNameAllocated(0, a).get()};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> shape{1, 3, 224, 224};
        std::vector<Ort::Value> in;
        in.push_back(Ort::Value::CreateTensor<float>(mem, px.data(), px.size(), shape.data(), 4));
        return run(*vision_, "image_embeds", inNames, in);
    }

    Ort::Env& env_;
    std::unique_ptr<Ort::Session> vision_, text_;
    std::vector<std::vector<float>> labels_, prompts_;
};

// MARK: The store

SceneTagStore::SceneTagStore(QObject* parent) : QObject(parent) { pool_.setMaxThreadCount(1); }

SceneTagStore::~SceneTagStore() {
    {
        std::lock_guard g(lock_);
        queue_.clear();
    }
    pool_.waitForDone();
}

QString SceneTagStore::modelFolder() {
    if (qEnvironmentVariableIsSet("EDITROOM_CLIP_MODEL")) return qEnvironmentVariable("EDITROOM_CLIP_MODEL");
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Models/clip-vit-base-patch32");
}

QStringList SceneTagStore::modelFiles() {
    return {QStringLiteral("vision_model_quantized.onnx"), QStringLiteral("text_model_quantized.onnx"), QStringLiteral("tokenizer.json")};
}

QString SceneTagStore::modelURL(const QString& file) {
    const QString base = QStringLiteral("https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/main/");
    return base + (file.endsWith(QStringLiteral(".onnx")) ? QStringLiteral("onnx/") : QString()) + file;
}

bool SceneTagStore::runtimeAvailable() { return onnx::load(); }

bool SceneTagStore::available() {
    for (const QString& f : modelFiles()) if (QFileInfo(modelFolder() + u'/' + f).size() < 1'000'000) return false;
    return true;
}

QString SceneTagStore::folder() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Scenes");
    QDir().mkpath(d);
    return d;
}

/// Named with the looker and the version of how tags are chosen, so a change makes them again.
QString SceneTagStore::file(const QString& path) const { return folder() + u'/' + WaveformReader::cacheName(path) + QStringLiteral("-clip1.json"); }

void SceneTagStore::downloadModel(std::function<void(double)> progress, std::function<void(QString)> done) {
    if (reply_) return;
    if (!net_) net_ = new QNetworkAccessManager(this);
    QDir().mkpath(modelFolder());
    fetched_ = 0;
    fetch(0, std::move(progress), std::move(done));
}

void SceneTagStore::fetch(int index, std::function<void(double)> progress, std::function<void(QString)> done) {
    const QStringList files = modelFiles();
    if (index >= files.size()) {
        emit modelInstalled();
        done(QString());
        next();
        return;
    }
    const QString target = modelFolder() + u'/' + files[index];
    if (QFileInfo(target).size() >= 1'000'000) {
        fetched_ += QFileInfo(target).size();
        fetch(index + 1, progress, done);
        return;
    }
    auto part = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!part->open(QIODevice::WriteOnly | QIODevice::Truncate)) { done(QStringLiteral("Couldn't save the scene model.")); return; }
    QNetworkRequest req{QUrl(modelURL(files[index]))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Editroom"));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this, part] { part->write(reply_->readAll()); });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this, progress](qint64 got, qint64) { progress(double(fetched_ + got) / double(modelBytes)); });
    connect(reply_, &QNetworkReply::finished, this, [this, part, target, index, progress, done] {
        part->write(reply_->readAll());
        part->close();
        const bool ok = reply_->error() == QNetworkReply::NoError && part->size() >= 1'000'000;
        const QString err = reply_->error() == QNetworkReply::OperationCanceledError ? QString()
                          : QStringLiteral("Couldn't download the scene model: %1").arg(reply_->errorString());
        reply_->deleteLater();
        reply_ = nullptr;
        if (!ok) { part->remove(); done(err); return; }
        fetched_ += part->size();
        QFile::remove(target);
        part->rename(target);
        fetch(index + 1, progress, done);
    });
}

void SceneTagStore::cancelDownload() { if (reply_) reply_->abort(); }

std::optional<SceneTags> SceneTagStore::load(const QString& path) {
    QFile f(file(path));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    auto t = SceneTags::fromJson(QJsonDocument::fromJson(f.readAll()).object());
    if (!t) return std::nullopt;
    std::lock_guard g(lock_);
    made_[path] = *t;
    return t;
}

void SceneTagStore::save(const SceneTags& t, const QString& path) {
    QSaveFile f(file(path));
    if (f.open(QIODevice::WriteOnly)) { f.write(json::write(t.json(), false)); f.commit(); }
}

std::optional<SceneTags> SceneTagStore::tags(const QString& path) {
    if (path.isEmpty()) return std::nullopt;
    {
        std::lock_guard g(lock_);
        if (auto it = made_.find(path); it != made_.end()) return it->second;
    }
    return load(path);
}

bool SceneTagStore::pending(const QString& path) const {
    std::lock_guard g(lock_);
    if (working_ == path) return true;
    return std::any_of(queue_.begin(), queue_.end(), [&](const Job& j) { return j.path == path; });
}

void SceneTagStore::forget(const QString& path) {
    {
        std::lock_guard g(lock_);
        made_.erase(path);
        std::erase_if(queue_, [&](const Job& j) { return j.path == path; });
        if (working_ == path) stale_.insert(path);
    }
    emit changed();
}

void SceneTagStore::make(const QString& path, bool isVideo, double duration) {
    if (path.isEmpty() || !available()) return;
    {
        std::lock_guard g(lock_);
        if (working_ == path && stale_.count(path)) { again_[path] = {path, isVideo, duration}; return; }
        if (made_.count(path) || working_ == path) return;
        if (std::any_of(queue_.begin(), queue_.end(), [&](const Job& j) { return j.path == path; })) return;
    }
    if (load(path)) return;
    {
        std::lock_guard g(lock_);
        queue_.push_back({path, isVideo, duration});
    }
    next();
}

void SceneTagStore::next() {
    Job job;
    {
        std::lock_guard g(lock_);
        if (working_ || queue_.empty() || !available()) return;
        job = queue_.front();
        queue_.pop_front();
        working_ = job.path;
    }
    pool_.start([this, job] {
        auto result = job.isVideo ? seeVideo(job.path, job.duration) : seePhoto(job.path);
        QMetaObject::invokeMethod(this, [this, job, result] {
            bool saved = false;
            {
                std::lock_guard g(lock_);
                working_.reset();
                if (stale_.erase(job.path)) {
                    if (auto it = again_.find(job.path); it != again_.end()) { queue_.push_back(it->second); again_.erase(it); }
                } else if (result) {
                    made_[job.path] = *result;
                    saved = true;
                }
            }
            if (saved) { save(*result, job.path); emit changed(); }
            next();
        }, Qt::QueuedConnection);
    });
}

void SceneTagStore::waitForDone() {
    for (;;) {
        {
            std::lock_guard g(lock_);
            if (!working_ && queue_.empty()) return;
        }
        pool_.waitForDone(20);
        QCoreApplication::processEvents();
    }
}

// MARK: Looking

std::vector<std::pair<QString, double>> SceneTagStore::classify(const QImage& image) {
    SceneLooker* l = SceneLooker::shared();
    if (!l) return {};
    try {
        return l->classify(image);
    } catch (const Ort::Exception&) {
        return {};
    }
}

std::optional<SceneTagStore::Aesthetics> SceneTagStore::aesthetics(const QImage& image) {
    if (!available()) return std::nullopt;
    SceneLooker* l = SceneLooker::shared();
    if (!l) return std::nullopt;
    static std::mutex m;
    std::lock_guard g(m);
    try {
        const auto made = l->compare(image, {0, 1});
        const auto kind = l->compare(image, {2, 3, 4});
        if (made.size() != 2 || kind.size() != 3) return std::nullopt;
        return Aesthetics{std::round((2 * made[0] - 1) * 1000) / 1000, kind[1] + kind[2] > 0.6};
    } catch (const Ort::Exception&) {
        return std::nullopt;
    }
}

namespace {
using Seen = std::vector<std::pair<double, std::vector<std::pair<QString, double>>>>;

/// Labels with a share of at least 0.02 in a frame (Vision's 0.2: the looker's shares are of every label).
std::vector<std::pair<QString, double>> keep(std::vector<std::pair<QString, double>> all) {
    std::erase_if(all, [](const auto& l) { return l.second < 0.02; });
    return all;
}

SceneTags gather(const Seen& seen) {
    std::map<QString, std::pair<double, std::vector<double>>> best;
    for (const auto& [t, labels] : seen) {
        for (const auto& [name, c] : labels) {
            auto& e = best[name];
            e.first = std::max(e.first, c);
            if (c >= SceneTagStore::sure) e.second.push_back(std::round(t * 100) / 100);
        }
    }
    SceneTags tags;
    for (const auto& [name, e] : best)
        if (e.first >= SceneTagStore::sure) tags.tags.push_back({name, std::round(e.first * 100) / 100, e.second});
    std::stable_sort(tags.tags.begin(), tags.tags.end(), [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
    return tags;
}
} // namespace

std::optional<SceneTags> SceneTagStore::seeVideo(const QString& path, double duration) {
    if (!SceneLooker::shared()) return std::nullopt;
    auto dec = VideoDecoder::open(path, VideoDecoder::Accel::software);
    if (!dec || duration <= 0) return std::nullopt;
    const int n = std::min(20, std::max(8, int(std::round(duration))));
    Seen seen;
    for (int i = 0; i < n; ++i) {
        const double t = duration * (i + 0.5) / n;
        auto f = dec->frameAt(t);
        if (!f) continue;
        seen.push_back({t, keep(classify(ScrubCache::image(*f, 640)))});
    }
    if (seen.empty()) return std::nullopt;
    return gather(seen);
}

std::optional<SceneTags> SceneTagStore::seePhoto(const QString& path) {
    if (!SceneLooker::shared()) return std::nullopt;
    auto img = Stills::load(path, 640);
    if (!img) return std::nullopt;
    return gather({{0.0, keep(classify(*img))}});
}

} // namespace er
