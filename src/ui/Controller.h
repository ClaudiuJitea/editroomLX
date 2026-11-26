// The app's wiring: the EditorModel's host (player, media, sound, dialogs), importing, the export queue,
// thumbnails, and rebuilding the player when the project changes. One per window.
#pragma once
#include "SourcePreview.h"
#include "Thumbnails.h"
#include "editor/EditorModel.h"
#include "engine/Exporter.h"
#include "audio/AudioScrubber.h"
#include "audio/Meters.h"
#include "engine/PlaybackEngine.h"
#include "engine/ScrubCache.h"
#include "engine/SceneTagStore.h"
#include "engine/Transcripts.h"
#include <QObject>

class QWidget;

namespace er {

class Controller : public QObject, public EditorHost {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    EditorModel& model() { return *model_; }
    MediaStore& media() { return media_; }
    SoundStore& sound() { return *sound_; }
    PlaybackEngine& engine() { return *engine_; }
    ExportQueue& exports() { return *exports_; }
    Thumbnails& thumbnails() { return *thumbs_; }
    /// Track and output levels, updated every frame (Meters.swift).
    const MeterState& meters() const { return meters_; }
    AudioScrubber& scrubber() { return scrubber_; }
    ScrubCache& scrubCache() { return scrubCache_; }
    /// What's said in each file (Transcripts.swift).
    Transcripts& transcripts() { return transcripts_; }
    /// What each clip and photo shows (SceneTagStore).
    SceneTagStore& scenes() { return scenes_; }
    std::optional<SceneTags> sceneTags(const Uuid& asset);
    /// Downloads the scene model with its progress showing (Settings).
    void downloadSceneModel();
    /// A library item or a trimmed edge's frame, shown in the viewer over the edit.
    SourcePreview& sourcePreview() { return *sourcePreview_; }
    void setWindow(QWidget* w) { window_ = w; }

    /// Imports files and folders (EditorModel.importFiles): in the background, one at a time.
    /// `then` gets the assets made, in order, once every file has been looked at (files already in the library
    /// count as theirs).
    void importFiles(const QStringList& paths, bool offerMatch = true, std::function<void(std::vector<MediaAsset>)> then = {});
    void showImportPanel();
    /// Brings the Media panel up to date with the disk: files added to linked folders come in. Run when
    /// Editroom comes back to the front, when a project opens, and from Reload Folder from Disk.
    void syncFolders(std::function<void()> then = {});
    /// Reload from Disk: opens items' files again as they are now. Clips keep their places and trims; a clip
    /// past the end of a file that got shorter is cut to fit (one undo step).
    void reloadMedia(const IdSet& ids);
    /// Reload Folder from Disk: new files come in, and everything in the folder is opened again.
    void reloadFolder(const QString& folder);
    /// Points an item at a file somewhere else (Locate…); others missing from the same folder are found too.
    bool relink(const Uuid& id, const QString& path);
    /// Points a linked folder, and everything in it, at where it is now.
    void relinkFolder(const QString& from, const QString& to);
    /// Shows a file or folder in the file manager.
    static void showInFileManager(const QString& path);
    int importCount() const { return importCount_; }
    QString importError() const { return importError_; }
    void clearImportError() { importError_.clear(); }
    void setImportError(const QString& e) { importError_ = e; emit importChanged(); }

    // EditorHost
    void rebuild(const Project& p, double playhead) override;
    void seek(double t, bool fast) override;
    void play() override;
    void pause() override;
    double currentTime() const override;
    bool engineEmpty() const override;
    bool showScrubFrame(double t) override;
    void makeTranscript(const Uuid& asset, bool soon) override;
    bool transcriptsAvailable() const override { return Transcripts::available(); }
    bool transcriptPending(const Uuid& asset) const override;
    bool offerSpeechModel() override;
    /// Downloads the speech model with its progress showing (Settings, or the offer).
    void downloadSpeechModel();
    void setLoopSpan(std::optional<Range> span) override;
    void setAudible(const std::map<Uuid, bool>& audible) override;
    std::optional<Transcript> transcript(const Uuid& asset) const override;
    void prepareReversedAudio(const Uuid& asset) override;
    void scrubSound(double t) override;
    void audioScrubChanged(bool on) override;
    std::optional<double> envelopeDB(const Clip& c) const override;
    std::optional<double> peakDB(const Clip& c) const override;
    void expect(const std::vector<MediaAsset>& assets) override;
    int alert(const QString& message, const QString& info, const QStringList& buttons) override;
    void showError(const QString& message) override;
    void locate(const Uuid& asset, bool thenOthers) override;
    void locateFolder(const Uuid& folder) override;
    QString chooseSaveFile(const QString& suggested) override;
    QString chooseProjectFile() override;

signals:
    void importChanged();
    void frame(const QImage& image, double time);
    /// The meters moved (emitted only when a reading changed visibly).
    void metersChanged();

private:
    void offerMatch(const MediaAsset& a);
    MediaStore media_;
    std::unique_ptr<SoundStore> sound_;
    std::unique_ptr<PlaybackEngine> engine_;
    std::unique_ptr<ExportQueue> exports_;
    std::unique_ptr<Thumbnails> thumbs_;
    std::unique_ptr<SourcePreview> sourcePreview_;
    std::unique_ptr<EditorModel> model_;
    QWidget* window_ = nullptr;
    int importCount_ = 0;
    QString importError_;
    QTimer tick_;
    /// Linked folders being brought in right now (kept while their files come in).
    std::set<Uuid> importingFolders_;
    std::set<QString> pendingImports_, unreadable_;
    bool syncingFolders_ = false;
    std::pair<std::optional<MediaFolder>, bool> linkFolder(const QString& path);
    MeterState meters_;
    ScrubCache scrubCache_;
    Transcripts transcripts_;
    SceneTagStore scenes_;
    /// Scene tags for the project's clips and photos, made in the background (once each).
    void makeSceneTags(const Project& p);
    /// Transcripts for the project's files with sound, made in the background (once each).
    void makeTranscripts(const Project& p);
    /// Made-ahead scrub frames for the project's standard-colour videos (once each).
    void makeScrubFrames(const Project& p);
    AudioScrubber scrubber_;
    void tick();
    void prepareScrubber();
};

/// "30 fps", "29.97 fps" (InspectorView.fpsLabel).
QString fpsLabel(double fps);

} // namespace er
