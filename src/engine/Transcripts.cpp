#include "Transcripts.h"
#include "audio/Waveform.h"
#include "media/Decoder.h"
#include "model/Codec.h"
#include "model/Json.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <whisper.h>

namespace er {

Transcripts::Transcripts(QObject* parent) : QObject(parent) { pool_.setMaxThreadCount(1); }

Transcripts::~Transcripts() {
    {
        std::lock_guard g(lock_);
        queue_.clear();
    }
    pool_.waitForDone();
}

QString Transcripts::modelPath() {
    // (Tests and developers can point at a model elsewhere.)
    if (qEnvironmentVariableIsSet("EDITROOM_WHISPER_MODEL")) return qEnvironmentVariable("EDITROOM_WHISPER_MODEL");
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Models/ggml-base.bin");
}

QString Transcripts::modelURL() { return QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"); }

bool Transcripts::available() {
    const QFileInfo f(modelPath());
    return f.exists() && f.size() > 1'000'000;
}

QString Transcripts::folder() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Transcripts");
    QDir().mkpath(d);
    return d;
}

QString Transcripts::file(const QString& path) const { return folder() + u'/' + WaveformReader::cacheName(path) + QStringLiteral(".json"); }

void Transcripts::downloadModel(std::function<void(double)> progress, std::function<void(QString)> done) {
    if (reply_) return;
    if (!net_) net_ = new QNetworkAccessManager(this);
    QDir().mkpath(QFileInfo(modelPath()).absolutePath());
    auto part = std::make_shared<QFile>(modelPath() + QStringLiteral(".part"));
    if (!part->open(QIODevice::WriteOnly | QIODevice::Truncate)) { done(QStringLiteral("Couldn't save the speech model.")); return; }
    QNetworkRequest req{QUrl(modelURL())};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // A generic name: nothing about the person goes with the request.
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Editroom"));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this, part] { part->write(reply_->readAll()); });
    connect(reply_, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) {
        progress(double(got) / double(total > 0 ? total : modelBytes));
    });
    connect(reply_, &QNetworkReply::finished, this, [this, part, done] {
        part->write(reply_->readAll());
        part->close();
        const bool ok = reply_->error() == QNetworkReply::NoError && part->size() > 1'000'000;
        const QString err = reply_->error() == QNetworkReply::OperationCanceledError ? QString()
                          : QStringLiteral("Couldn't download the speech model: %1").arg(reply_->errorString());
        reply_->deleteLater();
        reply_ = nullptr;
        if (!ok) { part->remove(); done(err); return; }
        QFile::remove(modelPath());
        part->rename(modelPath());
        emit modelInstalled();
        done(QString());
        next();
    });
}

void Transcripts::cancelDownload() { if (reply_) reply_->abort(); }

QString Transcripts::chosenLanguage() { return QSettings().value(QLatin1String(languageKey)).toString(); }

QString Transcripts::languageName(const QString& identifier) {
    const QString code = identifier.section(u'_', 0, 0).section(u'-', 0, 0).toLower();
    const QLocale l(code);
    if (l.language() == QLocale::C || (l.language() == QLocale::English && code != QLatin1String("en"))) return identifier;
    return QLocale::languageToString(l.language());
}

Transcripts::Language Transcripts::resolve() {
    const QString chosen = chosenLanguage();
    if (chosen.isEmpty()) return {QStringLiteral("auto"), QString()};
    const QString code = chosen.section(u'_', 0, 0).section(u'-', 0, 0).toLower();
    const QString name = languageName(chosen);
    // An English-only model hears only English.
    if (QFileInfo(modelPath()).fileName().contains(QLatin1String(".en"))) return {code == QLatin1String("en") ? std::optional<QString>(code) : std::nullopt, name};
    if (whisper_lang_id(code.toLatin1().constData()) < 0) return {std::nullopt, name};
    return {code, name};
}

std::vector<std::pair<QString, QString>> Transcripts::supportedLanguages() {
    std::vector<std::pair<QString, QString>> out;
    for (int i = 0; i <= whisper_lang_max_id(); ++i) {
        const QString code = QString::fromLatin1(whisper_lang_str(i));
        const QString name = languageName(code);
        // Only languages Qt can name (whisper also lists a few it can barely hear, by code).
        if (name != code) out.emplace_back(code, name);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return QString::localeAwareCompare(a.second, b.second) < 0; });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second == b.second; }), out.end());
    return out;
}

QString Transcripts::unsupportedLanguage() const {
    std::lock_guard g(lock_);
    return unsupportedLanguage_;
}

void Transcripts::languageChanged() {
    std::vector<QString> known;
    {
        std::lock_guard g(lock_);
        std::set<QString> all;
        for (const auto& [p, t] : made_) all.insert(p);
        for (const auto& [p, s] : states_) if (s != State::working) all.insert(p);
        known.assign(all.begin(), all.end());
        made_.clear();
        for (const auto& p : known) states_.erase(p);
        unsupportedLanguage_.clear();
    }
    for (const auto& p : known) { emit changed(p); make(p); }
}

std::optional<Transcript> Transcripts::load(const QString& path) {
    QFile f(file(path));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    auto t = codec::decodeTranscript(QJsonDocument::fromJson(f.readAll()).object());
    if (!t) return std::nullopt;
    // Heard in another language than the one now set: heard again.
    if (const auto now = resolve().code; now && *now != QLatin1String("auto") && !t->language.isEmpty() &&
        t->language.section(u'_', 0, 0).section(u'-', 0, 0).toLower() != *now) return std::nullopt;
    std::lock_guard g(lock_);
    made_[path] = *t;
    return t;
}

void Transcripts::save(const Transcript& t, const QString& path) {
    QSaveFile f(file(path));
    if (f.open(QIODevice::WriteOnly)) { f.write(json::write(codec::encode(t), false)); f.commit(); }
}

std::optional<Transcript> Transcripts::transcript(const QString& path) {
    {
        std::lock_guard g(lock_);
        if (auto it = made_.find(path); it != made_.end()) return it->second;
    }
    return load(path);
}

std::optional<Transcripts::State> Transcripts::state(const QString& path) const {
    std::lock_guard g(lock_);
    if (made_.count(path)) return State::ready;
    if (auto it = states_.find(path); it != states_.end()) return it->second;
    return std::nullopt;
}

void Transcripts::forget(const QString& path) {
    std::lock_guard g(lock_);
    made_.erase(path);
    states_.erase(path);
    if (working_ == path) stale_.insert(path);
}

void Transcripts::make(const QString& path, bool soon) {
    if (!available()) return;
    {
        std::lock_guard g(lock_);
        if (made_.count(path)) return;
        // Already found the language can't be heard: not tried again until it's changed (languageChanged).
        if (auto it = states_.find(path); it != states_.end() && it->second == State::unsupported) return;
        if (working_ == path && stale_.count(path)) { states_[path] = State::waiting; return; }
    }
    if (load(path)) {
        std::lock_guard g(lock_);
        states_[path] = State::ready;
        return;
    }
    {
        std::lock_guard g(lock_);
        if (working_ == path) return;
        auto it = std::find(queue_.begin(), queue_.end(), path);
        if (it != queue_.end()) {
            if (!soon) return;
            queue_.erase(it);
        }
        if (soon) queue_.push_front(path); else queue_.push_back(path);
        states_[path] = State::waiting;
    }
    emit changed(path);
    next();
}

void Transcripts::ready(const QString& path, std::function<void(std::optional<Transcript>)> then, double limit) {
    if (auto t = transcript(path)) { then(t); return; }
    if (!available() || state(path) == State::failed || state(path) == State::unsupported) { then(std::nullopt); return; }
    {
        std::lock_guard g(lock_);
        waiters_[path].push_back(then);
    }
    make(path, true);
    QTimer::singleShot(int(limit * 1000), this, [this, path] {
        std::vector<std::function<void(std::optional<Transcript>)>> list;
        {
            std::lock_guard g(lock_);
            auto it = waiters_.find(path);
            if (it == waiters_.end()) return;
            list = std::move(it->second);
            waiters_.erase(it);
        }
        for (auto& f : list) f(std::nullopt);
    });
}

void Transcripts::next() {
    QString path;
    {
        std::lock_guard g(lock_);
        if (working_ || queue_.empty() || !available()) return;
        path = queue_.front();
        queue_.pop_front();
        // Not a transcript in the wrong language: none, and said why.
        if (const auto lang = resolve(); !lang.code) {
            states_[path] = State::unsupported;
            unsupportedLanguage_ = lang.name;
            auto it = waiters_.find(path);
            auto list = it == waiters_.end() ? std::vector<std::function<void(std::optional<Transcript>)>>{} : std::move(it->second);
            if (it != waiters_.end()) waiters_.erase(it);
            QMetaObject::invokeMethod(this, [this, path, list] {
                for (auto& f : list) f(std::nullopt);
                emit changed(path);
                next();
            }, Qt::QueuedConnection);
            return;
        }
        working_ = path;
        states_[path] = State::working;
    }
    emit changed(path);
    QPointer<Transcripts> self(this);
    pool_.start([this, path] {
        auto result = transcribe(path);
        QMetaObject::invokeMethod(this, [this, path, result] {
            std::vector<std::function<void(std::optional<Transcript>)>> list;
            bool requeue = false;
            {
                std::lock_guard g(lock_);
                working_.reset();
                if (stale_.erase(path)) {
                    // Heard from the old file; the new one goes to the back of the line.
                    if (states_[path] == State::waiting) { queue_.push_back(path); requeue = true; }
                    else states_.erase(path);
                } else if (result) {
                    made_[path] = *result;
                    states_[path] = State::ready;
                } else {
                    states_[path] = State::failed;
                }
                if (!requeue) {
                    if (auto it = waiters_.find(path); it != waiters_.end()) { list = std::move(it->second); waiters_.erase(it); }
                }
            }
            if (result && !requeue) save(*result, path);
            for (auto& f : list) f(result);
            emit changed(path);
            next();
        }, Qt::QueuedConnection);
    });
}

void Transcripts::waitForDone() {
    for (;;) {
        {
            std::lock_guard g(lock_);
            if (!working_ && queue_.empty()) return;
        }
        pool_.waitForDone(20);
        QCoreApplication::processEvents();
    }
}

namespace {
/// whisper.cpp's model, loaded once and kept (the worker is the only user).
whisper_context* model() {
    static whisper_context* ctx = nullptr;
    static QString loaded;
    if (ctx && loaded == Transcripts::modelPath()) return ctx;
    if (ctx) { whisper_free(ctx); ctx = nullptr; }
    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
    auto cp = whisper_context_default_params();
    ctx = whisper_init_from_file_with_params(Transcripts::modelPath().toLocal8Bit().constData(), cp);
    loaded = Transcripts::modelPath();
    return ctx;
}
} // namespace

/// Every word in the file's sound, with its time. None if it couldn't be read; empty if nothing's said.
std::optional<Transcript> Transcripts::transcribe(const QString& path) {
    if (!available()) return std::nullopt;
    auto dec = AudioDecoder::open(path, WHISPER_SAMPLE_RATE);
    if (!dec) return Transcript{{}, QString()};
    std::vector<float> stereo;
    while (dec->read(stereo, 1 << 16) > 0) {}
    std::vector<float> mono(stereo.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
    if (mono.size() < size_t(WHISPER_SAMPLE_RATE / 2)) return Transcript{{}, QString()};
    whisper_context* ctx = model();
    if (!ctx) return std::nullopt;
    auto p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    p.translate = false;
    // The speech language from Settings, or heard from the speech itself (Automatic).
    const QByteArray language = resolve().code.value_or(QStringLiteral("auto")).toLatin1();
    p.language = language.constData();
    p.detect_language = false;
    p.n_threads = std::max(1, QThread::idealThreadCount() - 1);
    // One word a segment, with its time.
    p.token_timestamps = true;
    p.split_on_word = true;
    p.max_len = 1;
    p.suppress_blank = true;
    p.no_context = true;
    if (whisper_full(ctx, p, mono.data(), int(mono.size())) != 0) return std::nullopt;
    Transcript t;
    t.language = QString::fromLatin1(whisper_lang_str(whisper_full_lang_id(ctx)));
    const int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        QString text = QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed();
        // Not words: "[BLANK_AUDIO]", "(music)" and the like.
        if (text.isEmpty() || text.startsWith(u'[') || (text.startsWith(u'(') && text.endsWith(u')'))) continue;
        const double start = whisper_full_get_segment_t0(ctx, i) / 100.0, end = whisper_full_get_segment_t1(ctx, i) / 100.0;
        // A piece of a word (whisper splits some): joined to the word before.
        if (!t.words.empty() && !QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).startsWith(u' ') && text.front().isLetterOrNumber() == false) {
            t.words.back().text += text;
            t.words.back().end = end;
            continue;
        }
        t.words.push_back({start, std::max(end, start), text});
    }
    return t;
}

} // namespace er
