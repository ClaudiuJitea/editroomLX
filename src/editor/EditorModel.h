// EditorModel.swift: the open project, selection, undo and every editing command, ported literally.
//
// Undo works on whole-project snapshots, exactly as on the Mac: up to 200 steps; live changes (drags,
// sliders) merge into one step, committed 0.8 s after the last change; `amendLastStep` adds to the step
// before; the library, linked folders and Magic's prompt history are never undone, except that items a
// step deleted come back. There are no action names ("Undo", "Redo").
#pragma once
#include "EditorHost.h"
#include "TimelineLayout.h"
#include "model/Captions.h"
#include "model/Codec.h"
#include <QObject>
#include <QTimer>
#include <set>

namespace er {

/// What a drag from the Media panel (or a button) puts on the timeline.
struct DragPayload {
    enum class Kind { asset, solid, text } kind = Kind::solid;
    Uuid asset;
    static DragPayload ofAsset(const Uuid& id) { return {Kind::asset, id}; }
    static DragPayload solid() { return {Kind::solid, {}}; }
    static DragPayload text() { return {Kind::text, {}}; }
    bool operator==(const DragPayload&) const = default;
};

enum class Tool { move, blade };

/// Something picked in the Media panel: an item, or a folder (by path).
struct MediaPick {
    std::optional<Uuid> asset;
    QString folder;
    static MediaPick ofAsset(const Uuid& id) { return {id, {}}; }
    static MediaPick ofFolder(const QString& f) { return {std::nullopt, f}; }
    bool operator==(const MediaPick&) const = default;
};

class EditorModel : public QObject {
    Q_OBJECT
public:
    explicit EditorModel(EditorHost* host = nullptr, QObject* parent = nullptr);
    ~EditorModel() override;

    EditorHost* host() const { return host_; }
    void setHost(EditorHost* h) { host_ = h ? h : &nullHost_; }

    const Project& project() const { return project_; }
    QString fileName() const { return fileUrl_; }
    QString title() const;
    bool isDirty() const { return isDirty_; }
    bool canUndo() const { return !undoStack_.empty() || liveSnapshot_.has_value(); }
    bool canRedo() const { return !redoStack_.empty(); }
    size_t undoDepth() const { return undoStack_.size(); }

    // Selection
    const IdSet& selection() const { return selection_; }
    void setSelection(const IdSet& s);
    const std::set<QString>& captionSelection() const { return captionSelection_; }
    bool captionsFocused() const { return captionsFocused_; }
    IdSet mediaSelection;
    std::set<QString> folderSelection;
    std::optional<QString> mediaFolder;
    /// Where a Shift-click range in the Media panel starts.
    std::optional<MediaPick> mediaAnchor;
    /// A click on a Media panel item. `order` is the items as shown, for Shift-click ranges; Ctrl adds or takes
    /// away (⌘ on the Mac).
    void clickMedia(const MediaPick& pick, const std::vector<MediaPick>& order, Qt::KeyboardModifiers modifiers);
    /// Opens a folder in the Media panel (nullopt: the top).
    void openMediaFolder(const std::optional<QString>& folder);
    /// Nothing picked in Media (after a drop on the timeline, say: the new clips there are what's picked).
    void clearMediaPicks();
    /// The library item behind the clip selected on the timeline (the first, if several), for Reveal in Media.
    const MediaAsset* revealableAsset() const;
    /// Reveal in Media (Shift+F, as Final Cut's Reveal in Browser): the clip's file is opened in Media, in its
    /// folder, picked and scrolled to. The timeline's selection is left as it is.
    void revealInMedia(const std::optional<Uuid>& clipID = std::nullopt);

    // View settings kept in QSettings under the Mac's keys.
    Tool tool = Tool::move;
    bool snapping = true;
    bool skimming() const { return skimming_; }
    void setSkimming(bool on);
    bool thumbnails() const { return thumbnails_; }
    void setThumbnails(bool on);
    bool audioScrub() const { return audioScrub_; }
    void setAudioScrub(bool on);
    bool sticky() const { return sticky_; }
    void setSticky(bool on);
    bool loop() const { return loop_; }
    void setLoop(bool on);

    /// Only the viewer is shown, full screen (F; Esc leaves).
    bool viewerOnly() const { return viewerOnly_; }
    void toggleViewerFullScreen() { viewerOnly_ = !viewerOnly_; emit viewerOnlyChanged(viewerOnly_); }

    // Transport
    double playhead() const { return playhead_; }
    bool isPlaying() const { return isPlaying_; }
    std::optional<double> skimTime() const { return skimTime_; }
    void togglePlay();
    void play();
    void pause();
    void seek(double time);
    void scrub(double time);
    /// With Audio scrubbing on and nothing playing, a short burst of the edit's sound at `t`.
    void scrubSound(double t);
    /// When the last scrub sound was asked for (timeline time, and steady-clock seconds), so the meters can
    /// show what's being heard.
    std::optional<std::pair<double, double>> lastScrubSound() const { return lastScrubSound_; }
    void skim(std::optional<double> t);
    void step(int frames);
    void jumpToEdit(bool forward);
    /// What Loop repeats: the in/out range when there is one, otherwise the whole timeline.
    Range loopSpan() const;
    /// Called by the player when it reaches the end.
    void engineEnded();
    /// Called every display frame while playing (EditorModel.tick).
    void tick();
    static constexpr const char* playFromSkimmerKey = "playFromSkimmer";

    // Timeline zoom and scroll. Every change goes through `setView`, so the canvas, the scroll bars and the
    // wheel all agree on one set of limits and nothing can fight over the position.
    const TimelineViewState& view() const { return view_; }
    /// The timeline's size on screen (the canvas reports it).
    void setViewSize(double width, double height);
    /// Seconds of timeline that can be scrolled to: the content plus working room after it.
    double extent() const { return std::max(30.0, project_.duration() * 1.3 + 10); }
    /// Fully zoomed out shows exactly `extent`.
    double minPPS() const { return std::max(TimelineViewState::minPPS, view_.viewWidth / extent()); }
    double maxScrollX() const { return std::max(0.0, extent() * view_.pps - view_.viewWidth); }
    double rowsContent() const;
    double rowsVisible() const { return std::max(40.0, view_.viewHeight - TimelineLayout::ruler); }
    /// Distance from the top of the tracks to the middle of the gap between video and audio: the vertical
    /// "home" of the timeline, the centre of the scroll bar.
    double rowsAbove() const;
    /// The vertical bar is a window onto a taller "world" whose middle is the video/audio gap.
    double rowsWorld() const;
    /// Fraction of the vertical bar the thumb covers (now, or at another row height).
    double rowWindow() const { return rowsVisible() / rowsWorld(); }
    double rowWindow(double rowHeight) const;
    /// `scrollY` is how far the top of the tracks is above the top of the view; negative when they sit lower.
    double minScrollY() const { return rowsAbove() - rowsWorld() / 2; }
    double maxScrollY() const { return rowsAbove() + rowsWorld() / 2 - rowsVisible(); }
    double centeredScrollY() const { return rowsAbove() - rowsVisible() / 2; }
    /// A point in the tracks as fixed-height space above it plus rows' worth of height: `fixed + rows * rowHeight`.
    std::pair<double, double> rowAnchor(double y) const;
    void centerRows() { setView(std::nullopt, std::nullopt, centeredScrollY()); }
    /// How a timeline opens: rows as tall as fit with every track and the add-track zones in view, centred.
    void fitRows();
    void setView(std::optional<double> pps = std::nullopt, std::optional<double> scrollX = std::nullopt,
                 std::optional<double> scrollY = std::nullopt, std::optional<double> rowHeight = std::nullopt);
    void clampView() { setView(); }
    /// Zooms around the playhead: it keeps its place on screen, or comes to the centre if it was off screen.
    void setZoom(double pps);
    void zoom(double factor) { setZoom(view_.pps * factor); }
    void zoomToFit();

    // Mutation & undo
    void mutate(const std::function<void(Project&)>& body);
    void amendLastStep(const std::function<void(Project&)>& body);
    void live(const std::function<void(Project&)>& body, bool autoCommit = true);
    void discardLive();
    void commitLive();
    void undo();
    void redo();
    void setNeedsRebuild();
    void flushRebuild();
    bool rebuildSuspended() const { return rebuildSuspended_; }
    void setRebuildSuspended(bool s);

    // Markers and in/out
    /// The marker whose panel is open (its flag was clicked, or M was pressed on it).
    std::optional<Uuid> editingMarker;
    void setEditingMarker(std::optional<Uuid> id) { if (id != editingMarker) { editingMarker = id; emit editingMarkerChanged(); } }
    void addMarker();
    void updateMarker(const Uuid& id, const std::function<void(Marker&)>& change);
    void updateMarkerLive(const Uuid& id, const std::function<void(Marker&)>& change);
    void renameMarker(const Uuid& id, const QString& name);
    void setRangeEdgeLive(bool isIn, double t);
    void deleteMarker(const Uuid& id);
    void clearMarkers();
    void setInPoint();
    void setOutPoint();
    void clearRange();

    // Adding clips
    double payloadDuration(const DragPayload& p) const;
    std::vector<Clip> makeClips(const DragPayload& p, double start, const Uuid& videoTrack, const Uuid& audioTrack) const;
    void insert(const std::vector<Clip>& clips);
    void addAtPlayhead(const DragPayload& p);
    static const std::vector<double>& frameRates();
    static double standardFPS(double fps);

    // Editing commands
    void deleteSelection(bool ripple = false);
    /// Delete: the picked Media panel items if there are any, otherwise what's selected on the timeline.
    void deleteSelected(bool ripple = false);
    void removeAsset(const Uuid& id) { removeAssets({id}); }
    void removeAssets(const IdSet& picked, const std::set<QString>& folders = {});
    void removeFolder(const QString& folder) { removeAssets({}, {folder}); }
    void splitAtPlayhead();
    void rippleTrim(bool head);
    bool canRippleTrim() const;
    void toggleZoom(const Uuid& id);
    void showZoomed(const Uuid& id);
    /// Set while a speed or zoom dot is dragged in the inspector: the playhead, which follows the dot, shows
    /// its time on the timeline as when it's dragged itself.
    bool showPlayheadTime() const { return showPlayheadTime_; }
    void setShowPlayheadTime(bool on) { if (on != showPlayheadTime_) { showPlayheadTime_ = on; emit playheadChanged(playhead_); } }
    void split(const Uuid& clip, double t);
    void detachAudio();
    bool selectionIsLinked() const;
    void selectAll();

    // Captions
    std::vector<CaptionBlock> captionBlocks() const;
    int addCaptions(const std::optional<IdSet>& ids = std::nullopt);
    QString captionsMenuTitle() const;
    void captionsFromMenu();
    void removeCaptions(const std::optional<IdSet>& ids = std::nullopt);
    void removeCaptionTrack();
    bool hasCaptions(const IdSet& ids) const;
    void selectCaptions(const std::set<QString>& ids);
    void releaseCaptions();
    void focusCaptions();
    void deleteSelectedCaptions();
    void setCaptionText(const CaptionBlock& block, const QString& text);
    void typeCaptionText(const CaptionBlock& block, const QString& text);
    void setCaptionsHidden(bool hidden);
    /// Double-clicking a caption on the timeline: the inspector puts the cursor in its words.
    std::optional<QString> captionEditRequest;
    void requestCaptionEdit(const QString& id) { captionEditRequest = id; emit captionEditRequested(); }
    /// Clips that will have captions once what's said in them has been heard.
    bool captionsWaiting() const;
    void setCaptionStyle(const std::function<void(CaptionStyle&)>& change, bool live = false, bool settles = false);
    /// Bumped when a transcript becomes ready, so captions waiting for one appear.
    void transcriptsChanged();

    // Freeze, reverse, gaps
    void freezeFrame(double hold = 2);
    void reverseSelection();
    void deleteGap(const Uuid& track, double t);
    void closeAllGaps(const std::optional<std::vector<Uuid>>& tracks = std::nullopt);

    // Copy, paste, grade
    const std::vector<Clip>& clipboard() const { return clipboard_; }
    void copySelection();
    void cutSelection();
    void paste();
    void duplicateSelection();
    std::vector<Clip> placed(const std::vector<Clip>& clips, double t) const;
    bool canCopyGrade() const;
    bool canPasteGrade() const;
    void copyGrade();
    void pasteGrade();
    void evenOutLevels();
    std::map<Uuid, double> levelGains(const std::vector<Clip>& clips, bool matchOutput = false) const;
    void duck(const Uuid& track, const std::optional<Uuid>& voice, std::optional<double> amount = std::nullopt);
    void setTransition(const Uuid& into, const std::optional<Transition>& t);
    void addTransitionAtPlayhead();

    // What the Clip menu can do with what's picked
    bool canAddDissolve() const { return dissolveTarget().has_value(); }
    bool canDuplicate() const { return !selectedForCopy().empty(); }
    bool canReverse() const;
    bool canFreezeFrame() const { return freezeTarget().has_value(); }
    bool canDeleteSelected() const;
    bool canCaption() const;
    void resetPlayback(const IdSet& volume);
    void setSpeed(double speed);
    void addTrack(TrackKind kind);
    void removeTrack(const Uuid& id);
    void setSequence(int width, int height);
    const MediaAsset* matchableAsset() const;
    bool sequenceMatches(const MediaAsset& a) const;
    void matchSequence(const MediaAsset& a);
    void setFPS(double fps);
    void setExportTarget(const QString& name, const QString& directory);
    void addMagicPrompt(const QString& text, const QString& tab);
    const std::vector<MagicHistoryEntry>& magicHistory() const;

    // Files
    static const QStringList& projectExtensions();
    static bool isProjectFile(const QString& path);
    bool confirmDiscard(bool quitting = false);
    bool open(const QString& path);
    void newProject();
    void openProject();
    void save();
    void saveAs();
    bool writeTo(const QString& path);
    void load(const Project& p, const QString& path = {}, bool offerLocate = false);
    std::vector<MediaAsset> missingAssets() const;
    /// The missing-media alert: Locate… the folder or the files (after relinking, for what's still missing).
    void offerLocateMissing();
    QStringList recentProjects() const;
    void openRecent(const QString& path);
    void clearRecents();

    // Autosave and recovery
    static QString recoveryPath();
    static constexpr const char* autosaveKey = "autosave";
    static bool autosaves();
    void autosaveNow();
    void autosaveIfIdle();
    void dropUnsaved();
    void restoreLastSession();
    /// The 1 s autosave check (not in tests and `--selftest`, as on the Mac).
    void startAutosaveTimer() { autosaveTimer_.start(); }
    /// Test launches (`--selftest`, `--open`) don't change what opens next time (the Mac's rule).
    bool testLaunch = false;

    /// Applies a library change made outside undo (import, relink): the library isn't undone.
    void changeLibrary(const std::function<void(Project&)>& body, bool clearUndo = false);
    /// Items whose file was deleted from a linked folder (and that no clip uses) leave Media, as from the folder;
    /// Undo doesn't bring them back.
    void dropDeletedFromDisk(const IdSet& gone);

signals:
    void changed();
    void selectionChanged();
    void playheadChanged(double t);
    void playingChanged(bool playing);
    void errorRaised(const QString& message);
    void captionEditRequested();
    void viewerOnlyChanged(bool on);
    /// The Media panel's picks or open folder changed.
    void mediaSelectionChanged();
    /// Something the Media panel should scroll to (what was just imported, or revealed).
    void mediaRevealRequested(const MediaPick& target);
    void editingMarkerChanged();
    /// Zoom, scroll or row height changed.
    void viewChanged();

private:
    void didChange();
    void rebuildNow();
    void settleSoon();
    void noteRecent(const QString& path);
    void offerToLocateMissing();
    std::optional<std::tuple<double, double, IdSet>> rippleTrimRange(bool head) const;
    std::optional<Clip> freezeTarget() const;
    std::optional<Clip> dissolveTarget() const;
    std::vector<Clip> selectedForCopy() const;
    std::vector<Clip> gradableSelection() const;
    void setLastSession(const std::optional<QString>& v);
    std::optional<QString> lastSession() const;
    std::optional<QString> recoveryFor() const;
    void setRecoveryFor(const std::optional<QString>& v);
    static void keepFolders(Project& p, const Project& now, const std::set<Uuid>& importing);
    /// Items taken out because their file was deleted from a linked folder: Undo doesn't bring them back.
    std::set<Uuid> droppedFromDisk_;
    static std::vector<MediaAsset> restoring(const std::vector<MediaAsset>& earlier, const std::vector<MediaAsset>& now);

    EditorHost nullHost_;
    EditorHost* host_;
    Project project_;
    qint64 lastEditMs_ = 0;
    IdSet selection_;
    std::set<QString> captionSelection_;
    bool captionsFocused_ = false;
    std::vector<Project> undoStack_, redoStack_;
    std::vector<std::set<Uuid>> redoKnown_;
    std::optional<Project> liveSnapshot_;
    QTimer liveTimer_;
    QTimer autosaveTimer_;
    QTimer settleTimer_;
    bool rebuildPending_ = false;
    bool rebuildSuspended_ = false;
    qint64 lastRebuildMs_ = 0;
    double playhead_ = 0;
    bool isPlaying_ = false;
    bool showPlayheadTime_ = false;
    bool viewerOnly_ = false;
    TimelineViewState view_;
    double lastRowsAbove_ = 0;
    std::optional<std::pair<double, double>> lastScrubSound_;
    std::optional<double> skimTime_;
    bool skimming_ = true, thumbnails_ = true, audioScrub_ = false, sticky_ = false, loop_ = false;
    QString fileUrl_;
    bool isDirty_ = false;
    std::optional<Project> lastRecovered_;
    std::vector<Clip> clipboard_;
    /// A grade copied from a clip; the inner nullopt means the clip had no grade, so pasting clears grades.
    std::optional<std::optional<ColorGrade>> copiedGrade_;
    std::set<Uuid> importingFolders_;
    int transcriptVersion_ = 0;
    mutable std::optional<std::pair<QByteArray, std::vector<CaptionBlock>>> captionCache_;
    std::vector<MagicHistoryEntry> emptyHistory_;
};

} // namespace er
