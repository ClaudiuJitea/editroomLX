// Transcripts.swift: what's said in each file, each word with its time, made in the background one file at a
// time and kept on disk by file, so Magic, captions and Media search can use it at once. Heard on this computer
// with whisper.cpp (the Mac uses Apple's on-device speech recognition); nothing is sent anywhere. The speech
// model (about 150 MB) is downloaded once, only when the person asks for it (Settings › General, or when
// captions or a transcript are first wanted).
#pragma once
#include "model/Types.h"
#include <QObject>
#include <QThreadPool>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>

class QNetworkAccessManager;
class QNetworkReply;

namespace er {

class Transcripts : public QObject {
    Q_OBJECT
public:
    /// `unsupported`: the speech language (Settings › General) can't be heard by the speech model.
    enum class State { waiting, working, ready, failed, unsupported };

    /// The speech language Settings › General picked ("" for Automatic: heard from the speech itself). The Mac's key;
    /// a Mac's locale identifier ("es_ES") reads as its language.
    static constexpr const char* languageKey = "speechLanguage";
    static QString chosenLanguage();
    /// The language transcripts are made in: whisper's code for it ("es"; "auto" for Automatic), or none when the
    /// speech model can't hear it, with the language's name for saying so.
    struct Language { std::optional<QString> code; QString name; };
    static Language resolve();
    /// The languages the speech model can hear (code, name), for Settings, by name.
    static std::vector<std::pair<QString, QString>> supportedLanguages();
    /// A language's name, in English ("Spanish"), from a code or a locale identifier.
    static QString languageName(const QString& identifier);
    /// The speech language was changed in Settings: what was heard in another language is heard again.
    void languageChanged();
    /// The language's name when it can't be heard (once a file has been tried in it), for saying so.
    QString unsupportedLanguage() const;
    bool unsupported(const QString& path) const { return state(path) == State::unsupported; }

    explicit Transcripts(QObject* parent = nullptr);
    ~Transcripts() override;

    /// The speech model is on this computer.
    static bool available();
    static QString modelPath();
    /// Where the model comes from (whisper.cpp's multilingual base model).
    static QString modelURL();
    static constexpr qint64 modelBytes = 147951465;
    /// Downloads the model (`progress` 0…1, then `done` with an error message or none). Only when asked.
    void downloadModel(std::function<void(double)> progress, std::function<void(QString)> done);
    void cancelDownload();
    bool downloading() const { return reply_ != nullptr; }

    std::optional<Transcript> transcript(const QString& path);
    std::optional<State> state(const QString& path) const;
    /// Queues a file; `soon` puts it first (Magic is asking for it now).
    void make(const QString& path, bool soon = false);
    /// The transcript, made now if it isn't yet; `then` gets it (or none after `limit` seconds, or if it failed).
    void ready(const QString& path, std::function<void(std::optional<Transcript>)> then, double limit = 90);
    /// A file changed on disk: what was heard in it goes.
    void forget(const QString& path);
    /// Hears a file's speech now, on this thread (tests, and the worker).
    static std::optional<Transcript> transcribe(const QString& path);
    static QString folder();
    /// Waits until nothing is queued or being heard (tests).
    void waitForDone();

signals:
    void changed(const QString& path);
    void modelInstalled();

private:
    QString file(const QString& path) const;
    std::optional<Transcript> load(const QString& path);
    void save(const Transcript& t, const QString& path);
    void next();
    mutable std::mutex lock_;
    std::map<QString, Transcript> made_;
    std::map<QString, State> states_;
    std::deque<QString> queue_;
    std::optional<QString> working_;
    QString unsupportedLanguage_;
    std::set<QString> stale_;
    std::map<QString, std::vector<std::function<void(std::optional<Transcript>)>>> waiters_;
    QThreadPool pool_;
    QNetworkAccessManager* net_ = nullptr;
    QNetworkReply* reply_ = nullptr;
};

} // namespace er
