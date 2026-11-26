#include "Controller.h"
#include "Alert.h"
#include "audio/AudioPlan.h"
#include "model/MediaFolders.h"
#include <QFileDialog>
#include <QGuiApplication>
#include <QPointer>
#include <QProgressDialog>
#include <QThreadPool>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QJsonArray>
#include <QJsonDocument>
#include "model/Codec.h"
#include <chrono>
#include <cmath>

namespace er {

QString fpsLabel(double fps) {
    return (std::abs(fps - std::round(fps)) < 0.001 ? QString::number(int(fps)) : QString::asprintf("%.2f", fps)) + QStringLiteral(" fps");
}

Controller::Controller(QObject* parent) : QObject(parent) {
    sound_ = std::make_unique<SoundStore>(media_);
    engine_ = std::make_unique<PlaybackEngine>(media_, *sound_);
    exports_ = std::make_unique<ExportQueue>(media_, *sound_);
    thumbs_ = std::make_unique<Thumbnails>(media_);
    sourcePreview_ = std::make_unique<SourcePreview>(media_);
    model_ = std::make_unique<EditorModel>(this);
    connect(engine_.get(), &PlaybackEngine::frameReady, this, &Controller::frame);
    connect(engine_.get(), &PlaybackEngine::ended, model_.get(), &EditorModel::engineEnded);
    // Waveforms and reversed sound arriving change what's heard (ducking, reversed clips): rebuild.
    connect(sound_.get(), &SoundStore::waveformReady, model_.get(), [this] { model_->setNeedsRebuild(); });
    connect(sound_.get(), &SoundStore::reversedReady, model_.get(), [this] { model_->setNeedsRebuild(); });
    connect(&media_, &MediaStore::stillLoaded, model_.get(), [this] { model_->setNeedsRebuild(); });
    tick_.setInterval(16);
    connect(&tick_, &QTimer::timeout, this, &Controller::tick);
    if (model_->audioScrub()) scrubber_.warmUp();
    engine_->setScrubFrames([this](const Uuid& id, double t) { return scrubCache_.frame(id, t); },
                            [this](const Uuid& id) { return scrubCache_.has(id); });
    connect(&media_, &MediaStore::visualChanged, this, [this] { makeScrubFrames(model_->project()); });
    // What's said, for captions, Magic and Media search: read through the media store, and when one is ready the
    // captions waiting for it appear.
    media_.setTranscripts([this](const QString& path) { return transcripts_.transcript(path); });
    connect(&transcripts_, &Transcripts::changed, model_.get(), [this](const QString&) { model_->transcriptsChanged(); });
    connect(&transcripts_, &Transcripts::modelInstalled, this, [this] { makeTranscripts(model_->project()); });
    connect(&scenes_, &SceneTagStore::modelInstalled, this, [this] { makeSceneTags(model_->project()); });
    // New tags: Media search may find more.
    connect(&scenes_, &SceneTagStore::changed, this, &Controller::importChanged);
    sourcePreview_->quick = [this](const Uuid& id, double t) -> std::optional<QImage> {
        auto f = scrubCache_.frame(id, t);
        if (!f) return std::nullopt;
        const MediaRuntime* rt = media_.runtime(id);
        f->orientation = rt ? rt->orientation : 1;
        return Thumbnails::fit(*f, SourceColor::standard, 1920);
    };
    // Coming back to the front: files added to linked folders come in.
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) { if (s == Qt::ApplicationActive) syncFolders(); });
    tick_.start();
}

Controller::~Controller() {
    // Background imports, folder scans and reloads report back to this object: let them finish first.
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::removePostedEvents(this);
}

void Controller::rebuild(const Project& p, double playhead) {
    sound_->warm(p.assets);
    engine_->rebuild(p, playhead);
    makeScrubFrames(p);
    // A few seconds on, so opening a project stays quick.
    QTimer::singleShot(3000, this, [this] { makeTranscripts(model_->project()); });
    if (model_->audioScrub()) prepareScrubber();
}

static double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void Controller::tick() {
    model_->tick();
    const Project& p = model_->project();
    auto waveforms = [this](const Uuid& id) { return sound_->waveform(id, false); };
    std::map<Uuid, MeterReading> before = meters_.tracks();
    const MeterReading masterBefore = meters_.master();
    // While scrubbing with sound, the meters show what's being heard.
    const auto s = model_->lastScrubSound();
    if (!model_->isPlaying() && s && steadyNow() - s->second < AudioScrubber::snippet + 0.05)
        meters_.update(p, waveforms, s->first, true, steadyNow());
    else
        meters_.update(p, waveforms, model_->playhead(), model_->isPlaying(), steadyNow());
    bool moved = meters_.master().differs(masterBefore) || before.size() != meters_.tracks().size();
    for (const auto& [id, r] : meters_.tracks()) {
        auto it = before.find(id);
        if (it == before.end() || r.differs(it->second)) { moved = true; break; }
    }
    if (moved) emit metersChanged();
}

/// The scrub mix, made again when the edit's audio changes (what's heard: the audio tracks, the sound clips,
/// and which files' sound and reversed sound are ready).
void Controller::prepareScrubber() {
    const Project& p = model_->project();
    Project audio;
    audio.audioTracks = p.audioTracks;
    for (const auto& c : p.clips) if (c.kind == ClipKind::audio) audio.clips.push_back(c);
    QJsonArray print;
    print.append(codec::encode(audio));
    for (const auto& a : p.assets) {
        if (a.kind == MediaKind::image) continue;
        const MediaRuntime* rt = media_.runtime(a.id);
        print.append(a.id.toString() + (rt ? u'1' : u'0') + (sound_->reversed(a.id) ? u'1' : u'0'));
    }
    scrubber_.prepare(p, sound_->sources(), qHash(QJsonDocument(print).toJson(QJsonDocument::Compact)));
}

void Controller::scrubSound(double t) { scrubber_.play(t); }

void Controller::audioScrubChanged(bool on) {
    if (on) {
        scrubber_.warmUp();
        prepareScrubber();
    } else {
        scrubber_.shutDown();
    }
}

void Controller::seek(double t, bool fast) { engine_->seek(t, fast); }
bool Controller::showScrubFrame(double t) { return engine_->showScrubFrame(t); }

bool Controller::offerSpeechModel() {
    if (transcripts_.downloading()) return true;
    if (alert(QStringLiteral("Download the speech model?"),
              QStringLiteral("Captions, transcripts and searching what's said need Editroom's speech model. It's about 150 MB, downloaded once, "
                             "and runs on this computer: nothing you edit is sent anywhere."),
              {QStringLiteral("Download"), QStringLiteral("Not Now")}) != 0) return false;
    downloadSpeechModel();
    return true;
}

void Controller::downloadSpeechModel() {
    if (transcripts_.downloading()) return;
    auto* progress = new QProgressDialog(QStringLiteral("Downloading the speech model…"), QStringLiteral("Cancel"), 0, 1000, window_);
    progress->setWindowTitle(QStringLiteral("Speech"));
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QProgressDialog::canceled, this, [this] { transcripts_.cancelDownload(); });
    progress->show();
    QPointer<QProgressDialog> p(progress);
    transcripts_.downloadModel([p](double f) { if (p) p->setValue(int(std::clamp(f, 0.0, 1.0) * 1000)); },
                               [this, p](QString err) {
                                   if (p) p->close();
                                   if (!err.isEmpty()) showError(err);
                                   emit importChanged();
                               });
}

bool Controller::transcriptPending(const Uuid& asset) const {
    const auto s = const_cast<Transcripts&>(transcripts_).state(media_.soundPath(asset));
    return s == Transcripts::State::waiting || s == Transcripts::State::working;
}

void Controller::makeTranscript(const Uuid& asset, bool soon) {
    const QString path = media_.soundPath(asset);
    if (!path.isEmpty()) transcripts_.make(path, soon);
}

void Controller::makeTranscripts(const Project& p) {
    if (!Transcripts::available()) return;
    for (const auto& a : p.assets) {
        if (a.kind == MediaKind::image || !a.hasAudio || media_.looksOffline(a)) continue;
        const QString path = media_.soundPath(a.id);
        if (!path.isEmpty() && !transcripts_.state(path)) transcripts_.make(path);
    }
}

void Controller::makeScrubFrames(const Project& p) {
    for (const auto& a : p.assets) {
        if (a.kind != MediaKind::video || scrubCache_.has(a.id)) continue;
        const MediaRuntime* rt = media_.runtime(a.id);
        if (rt && rt->color == SourceColor::standard && !media_.looksOffline(a)) scrubCache_.make(a.id, media_.readPath(a), a.duration);
    }
    // Once a file is open, what it shows too.
    makeSceneTags(p);
}

void Controller::makeSceneTags(const Project& p) {
    if (!SceneTagStore::available()) return;
    for (const auto& a : p.assets) {
        if (a.kind == MediaKind::audio || media_.looksOffline(a) || !media_.runtime(a.id)) continue;
        scenes_.make(media_.readPath(a), a.kind == MediaKind::video, a.duration);
    }
}

std::optional<SceneTags> Controller::sceneTags(const Uuid& asset) {
    const MediaAsset* a = model_->project().asset(asset);
    if (!a || a->kind == MediaKind::audio) return std::nullopt;
    return scenes_.tags(media_.readPath(*a));
}

void Controller::downloadSceneModel() {
    if (scenes_.downloading()) return;
    auto* progress = new QProgressDialog(QStringLiteral("Downloading the scene model…"), QStringLiteral("Cancel"), 0, 1000, window_);
    progress->setWindowTitle(QStringLiteral("Scenes"));
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QProgressDialog::canceled, this, [this] { scenes_.cancelDownload(); });
    progress->show();
    QPointer<QProgressDialog> pd(progress);
    scenes_.downloadModel([pd](double f) { if (pd) pd->setValue(int(std::clamp(f, 0.0, 1.0) * 1000)); },
                          [this, pd](QString err) {
                              if (pd) pd->close();
                              if (!err.isEmpty()) showError(err);
                              emit importChanged();
                          });
}
void Controller::play() { engine_->play(); }
void Controller::pause() { engine_->pause(); }
double Controller::currentTime() const { return engine_->currentTime(); }
bool Controller::engineEmpty() const { return engine_->isEmpty(); }
void Controller::setLoopSpan(std::optional<Range> span) { engine_->setLoopSpan(span); }
void Controller::setAudible(const std::map<Uuid, bool>& audible) { engine_->setAudible(audible); }
std::optional<Transcript> Controller::transcript(const Uuid& asset) const { return media_.transcript(asset); }
void Controller::prepareReversedAudio(const Uuid& asset) { sound_->prepareReversed(asset); }

std::optional<double> Controller::envelopeDB(const Clip& c) const {
    if (!c.assetID) return std::nullopt;
    const Waveform* w = sound_->waveform(*c.assetID, false);
    return w ? AudioLevels::envelopeDB(c, w->peaks, w->peaksPerSecond) : std::nullopt;
}

std::optional<double> Controller::peakDB(const Clip& c) const {
    if (!c.assetID) return std::nullopt;
    const Waveform* w = sound_->waveform(*c.assetID, false);
    return w ? AudioLevels::peakDB(c, w->peaks, w->peaksPerSecond) : std::nullopt;
}

void Controller::expect(const std::vector<MediaAsset>& assets) {
    media_.expect(assets);
    // The project's files open in the background; the player rebuilds as they come.
    std::vector<MediaAsset> copy = assets;
    (void)QtConcurrent::run([this, copy] {
        media_.restore(copy);
        QMetaObject::invokeMethod(model_.get(), [this] { model_->setNeedsRebuild(); emit importChanged(); syncFolders(); }, Qt::QueuedConnection);
    });
}

int Controller::alert(const QString& message, const QString& info, const QStringList& buttons) {
    return Alert::run(window_, message, info, buttons);
}

void Controller::showError(const QString& message) { Alert::run(window_, message, QString(), {QStringLiteral("OK")}); }

QString Controller::chooseSaveFile(const QString& suggested) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QFileDialog::getSaveFileName(window_, QString(), dir + u'/' + suggested, QStringLiteral("Editroom project (*.room)"));
}

QString Controller::chooseProjectFile() {
    return QFileDialog::getOpenFileName(window_, QString(), QString(), QStringLiteral("Editroom project (*.room *.cutplay)"));
}

void Controller::showImportPanel() {
    QFileDialog d(window_);
    d.setFileMode(QFileDialog::ExistingFiles);
    d.setLabelText(QFileDialog::Accept, QStringLiteral("Import"));
    if (d.exec() != QDialog::Accepted) return;
    importFiles(d.selectedFiles());
}

void Controller::offerMatch(const MediaAsset& asset) {
    const Project& p = model_->project();
    if (asset.kind != MediaKind::video || p.settingsLocked || asset.width <= 0 || asset.height <= 0) return;
    const double fps = EditorModel::standardFPS(asset.fps);
    model_->changeLibrary([](Project& q) { q.settingsLocked = true; });
    if (asset.width == p.width && asset.height == p.height && fps == p.fps) return;
    const QString info = QStringLiteral("%1 × %2 · %3\nThe sequence is currently %4 × %5 · %6.")
        .arg(asset.width).arg(asset.height).arg(fpsLabel(fps)).arg(p.width).arg(p.height).arg(fpsLabel(p.fps));
    if (alert(QStringLiteral("Match the sequence to “%1”?").arg(asset.name), info, {QStringLiteral("Match"), QStringLiteral("Keep Current")}) != 0) return;
    model_->mutate([&](Project& q) { q.width = asset.width; q.height = asset.height; q.fps = fps; });
}

} // namespace er
