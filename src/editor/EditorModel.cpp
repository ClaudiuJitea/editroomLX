#include "EditorModel.h"
#include "model/Json.h"
#include "model/MediaFolders.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <chrono>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace er {

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

static bool boolSetting(const char* key, bool def) {
    QSettings s;
    return s.contains(QLatin1String(key)) ? s.value(QLatin1String(key)).toBool() : def;
}
static void setBoolSetting(const char* key, bool v) { QSettings().setValue(QLatin1String(key), v); }

EditorModel::EditorModel(EditorHost* host, QObject* parent) : QObject(parent), host_(host ? host : &nullHost_) {
    static quint64 serials = 0;
    serial_ = ++serials;
    skimming_ = boolSetting("skimming", true);
    thumbnails_ = boolSetting("thumbnails", true);
    audioScrub_ = boolSetting("audioScrub", false);
    sticky_ = boolSetting("stickyEdges", false);
    loop_ = boolSetting("loopPlayback", false);
    liveTimer_.setSingleShot(true);
    liveTimer_.setInterval(800);
    connect(&liveTimer_, &QTimer::timeout, this, [this] { commitLive(); });
    settleTimer_.setSingleShot(true);
    settleTimer_.setInterval(90);
    connect(&settleTimer_, &QTimer::timeout, this, [this] {
        if (isPlaying_) return;
        host_->endScrubFrames();
        host_->seek(skimTime_.value_or(playhead_));
    });
    autosaveTimer_.setInterval(1000);
    connect(&autosaveTimer_, &QTimer::timeout, this, [this] { autosaveIfIdle(); });
}

EditorModel::~EditorModel() = default;

QString EditorModel::title() const {
    if (fileUrl_.isEmpty()) return QStringLiteral("Untitled");
    return QFileInfo(fileUrl_).completeBaseName();
}

void EditorModel::setSelection(const IdSet& s) {
    if (s == selection_) return;
    selection_ = s;
    if (!selection_.empty() && (!captionSelection_.empty() || captionsFocused_)) {
        captionSelection_.clear();
        captionsFocused_ = false;
    }
    // Clips picked on the timeline (a click, Select All, a drop): Media lets go of its picks, so Delete is for the clips.
    if (!selection_.empty()) clearMediaPicks();
    emit selectionChanged();
}

void EditorModel::setSkimming(bool on) {
    skimming_ = on;
    setBoolSetting("skimming", on);
    if (!on) skim(std::nullopt);
}
void EditorModel::setThumbnails(bool on) { thumbnails_ = on; setBoolSetting("thumbnails", on); emit changed(); }
void EditorModel::setAudioScrub(bool on) {
    audioScrub_ = on;
    setBoolSetting("audioScrub", on);
    host_->audioScrubChanged(on);
}
void EditorModel::setSticky(bool on) { sticky_ = on; setBoolSetting("stickyEdges", on); }
void EditorModel::setLoop(bool on) { loop_ = on; setBoolSetting("loopPlayback", on); }

void EditorModel::reloadSharedSettings() {
    skimming_ = boolSetting("skimming", true);
    thumbnails_ = boolSetting("thumbnails", true);
    audioScrub_ = boolSetting("audioScrub", false);
    sticky_ = boolSetting("stickyEdges", false);
    loop_ = boolSetting("loopPlayback", false);
}

// MARK: Autosave and recovery

QString EditorModel::recoveryPath(const QString& slot, bool test) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Recovery");
    QDir().mkpath(dir);
    if (test) return dir + QStringLiteral("/Untitled (test).room");
    return dir + (slot.isEmpty() ? QStringLiteral("/Untitled.room") : QStringLiteral("/Untitled %1.room").arg(slot));
}
QString EditorModel::recoveryPath() const { return recoveryPath(slot_, testLaunch); }

std::optional<QString> EditorModel::savedSession(const QString& slot) {
    QSettings s;
    const QString k = slotKey(slot, QStringLiteral("lastSession"));
    if (!s.contains(k)) return std::nullopt;
    return s.value(k).toString();
}
std::optional<QString> EditorModel::savedRecoveryFor(const QString& slot) {
    QSettings s;
    const QString k = slotKey(slot, QStringLiteral("recoveryFor"));
    if (!s.contains(k)) return std::nullopt;
    return s.value(k).toString();
}

std::optional<QString> EditorModel::lastSession() const { return savedSession(slot_); }
void EditorModel::setLastSession(const std::optional<QString>& v) {
    if (testLaunch) return;
    QSettings s;
    const QString k = slotKey(slot_, QStringLiteral("lastSession"));
    if (v) s.setValue(k, *v); else s.remove(k);
}
std::optional<QString> EditorModel::recoveryFor() const { return savedRecoveryFor(slot_); }
void EditorModel::setRecoveryFor(const std::optional<QString>& v) {
    if (testLaunch) return;
    QSettings s;
    const QString k = slotKey(slot_, QStringLiteral("recoveryFor"));
    if (v) s.setValue(k, *v); else s.remove(k);
}

bool EditorModel::isBlank() const {
    return fileUrl_.isEmpty() && project_.clips.empty() && project_.assets.empty() && (!project_.folders || project_.folders->empty());
}

bool EditorModel::closeProject() {
    if (isBlank()) return true;
    if (!confirmDiscard()) return false;
    load(Project(), QString());
    QFile::remove(recoveryPath());
    setRecoveryFor(std::nullopt);
    lastRecovered_.reset();
    setLastSession(std::nullopt);
    emit changed();
    return true;
}

void EditorModel::forgetSlot() {
    QFile::remove(recoveryPath());
    if (testLaunch) return;
    QSettings s;
    s.remove(slotKey(slot_, QStringLiteral("lastSession")));
    s.remove(slotKey(slot_, QStringLiteral("recoveryFor")));
    autosaveTimer_.stop();
}

std::function<void()>& EditorModel::newTabHandler() { static std::function<void()> f; return f; }
std::function<void(const QString&)>& EditorModel::openHandler() { static std::function<void(const QString&)> f; return f; }

EditorModel::Clipboard& EditorModel::sharedClipboard() { static Clipboard c; return c; }

bool EditorModel::autosaves() { return boolSetting(autosaveKey, true); }

static bool writeAtomic(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(data);
    return f.commit();
}

void EditorModel::autosaveIfIdle() {
    if (!isDirty_ || liveSnapshot_ || nowMs() - lastEditMs_ <= 2000) return;
    autosaveNow();
}

void EditorModel::autosaveNow() {
    commitLive();
    if (!fileUrl_.isEmpty() && autosaves()) {
        if (!isDirty_) return;
        writeTo(fileUrl_);
        setLastSession(fileUrl_);
    } else if (!fileUrl_.isEmpty()) {
        // Auto-save off: the file is left alone, and the changes wait in the recovery copy.
        if (!isDirty_ || lastRecovered_ == project_) return;
        if (writeAtomic(recoveryPath(), codec::compactBytes(SavedDocument{1, project_, {}}))) {
            lastRecovered_ = project_;
            setRecoveryFor(fileUrl_);
            setLastSession(fileUrl_);
        }
    } else if (project_.clips.empty() && project_.assets.empty()) {
        QFile::remove(recoveryPath());
        if (lastSession() == QStringLiteral("untitled")) setLastSession(std::nullopt);
    } else if (isDirty_ && lastRecovered_ != project_) {
        if (writeAtomic(recoveryPath(), codec::compactBytes(SavedDocument{1, project_, {}}))) {
            lastRecovered_ = project_;
            setRecoveryFor(std::nullopt);
            setLastSession(QStringLiteral("untitled"));
        }
    }
}

void EditorModel::dropUnsaved() {
    QFile::remove(recoveryPath());
    setRecoveryFor(std::nullopt);
    lastRecovered_.reset();
    isDirty_ = false;
    if (fileUrl_.isEmpty() && lastSession() == QStringLiteral("untitled")) setLastSession(std::nullopt);
    emit changed();
}

static std::optional<SavedDocument> readDocument(const QString& path, QString* error = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    auto r = codec::decodeDocument(f.readAll());
    if (error) *error = r.error;
    return r.document;
}

void EditorModel::restoreLastSession() {
    const auto last = lastSession();
    if (!last) return;
    if (*last == QStringLiteral("untitled")) {
        auto doc = readDocument(recoveryPath());
        if (!doc) return;
        load(doc->project, QString(), true);
        lastRecovered_ = doc->project;
        isDirty_ = true;
    } else if (QFileInfo::exists(*last)) {
        open(*last);
        // Unsaved changes to it from last time (Auto-save off) come back, still unsaved.
        if (recoveryFor() == *last) {
            if (auto doc = readDocument(recoveryPath())) {
                doc->project.resolveMovedMedia(QFileInfo(*last).absolutePath());
                load(doc->project, *last, true);
                lastRecovered_ = doc->project;
                isDirty_ = true;
            }
        }
    }
    emit changed();
}

bool EditorModel::open(const QString& path) {
    auto doc = readDocument(path);
    if (!doc) {
        const QString msg = QStringLiteral("Couldn't open %1.").arg(QFileInfo(path).fileName());
        host_->showError(msg);
        emit errorRaised(msg);
        return false;
    }
    doc->project.resolveMovedMedia(QFileInfo(path).absolutePath());
    load(doc->project, QFileInfo(path).absoluteFilePath(), true);
    noteRecent(QFileInfo(path).absoluteFilePath());
    setLastSession(QFileInfo(path).absoluteFilePath());
    return true;
}

// MARK: Mutation & undo

void EditorModel::mutate(const std::function<void(Project&)>& body) {
    commitLive();
    Project p = project_;
    body(p);
    p.sanitize();
    if (p == project_) return;
    undoStack_.push_back(project_);
    if (undoStack_.size() > 200) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
    redoKnown_.clear();
    project_ = std::move(p);
    lastEditMs_ = nowMs();
    didChange();
}

void EditorModel::amendLastStep(const std::function<void(Project&)>& body) {
    commitLive();
    if (undoStack_.empty()) return mutate(body);
    Project p = project_;
    body(p);
    p.sanitize();
    if (p == project_) return;
    redoStack_.clear();
    redoKnown_.clear();
    project_ = std::move(p);
    lastEditMs_ = nowMs();
    didChange();
}

void EditorModel::live(const std::function<void(Project&)>& body, bool autoCommit) {
    Project p = project_;
    body(p);
    if (p == project_) return;
    if (!liveSnapshot_) liveSnapshot_ = project_;
    project_ = std::move(p);
    lastEditMs_ = nowMs();
    isDirty_ = true;
    setNeedsRebuild();
    liveTimer_.stop();
    if (autoCommit) liveTimer_.start();
    emit changed();
}

void EditorModel::discardLive() {
    liveTimer_.stop();
    if (!liveSnapshot_) return;
    project_ = *liveSnapshot_;
    liveSnapshot_.reset();
    setNeedsRebuild();
    emit changed();
}

void EditorModel::commitLive() {
    liveTimer_.stop();
    if (!liveSnapshot_) return;
    Project snapshot = std::move(*liveSnapshot_);
    liveSnapshot_.reset();
    if (snapshot != project_) {
        undoStack_.push_back(std::move(snapshot));
        redoStack_.clear();
        redoKnown_.clear();
    }
}

void EditorModel::undo() {
    commitLive();
    if (undoStack_.empty()) return;
    Project prev = std::move(undoStack_.back());
    undoStack_.pop_back();
    redoStack_.push_back(project_);
    std::set<Uuid> known;
    for (const auto& a : prev.assets) known.insert(a.id);
    redoKnown_.push_back(known);
    // The library isn't undone (imports and relinks stay), except that items a step deleted come back.
    prev.assets = restoring(prev.assets, project_.assets);
    // Except items whose file was deleted from a linked folder, unless this step's clips use them.
    {
        std::set<Uuid> usedThen;
        for (const auto& c : prev.clips) if (c.assetID) usedThen.insert(*c.assetID);
        std::erase_if(prev.assets, [&](const MediaAsset& a) { return droppedFromDisk_.count(a.id) && !usedThen.count(a.id); });
    }
    keepFolders(prev, project_, importingFolders_);
    // Magic's prompt history isn't undone either.
    prev.magicHistory = project_.magicHistory;
    project_ = std::move(prev);
    didChange();
}

void EditorModel::clearMediaPicks() {
    if (mediaSelection.empty() && folderSelection.empty()) return;
    mediaSelection.clear();
    folderSelection.clear();
    emit mediaSelectionChanged();
}

const MediaAsset* EditorModel::revealableAsset() const {
    std::vector<const Clip*> picked;
    for (const auto& c : project_.clips) if (selection_.count(c.id)) picked.push_back(&c);
    std::stable_sort(picked.begin(), picked.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    for (const Clip* c : picked) if (c->assetID) if (const MediaAsset* a = project_.asset(c->assetID)) return a;
    return nullptr;
}

void EditorModel::revealInMedia(const std::optional<Uuid>& clipID) {
    const MediaAsset* asset = nullptr;
    if (clipID) if (const Clip* c = project_.clip(*clipID); c && c->assetID) asset = project_.asset(c->assetID);
    if (!asset) asset = revealableAsset();
    if (!asset) return;
    const Uuid id = asset->id;
    openMediaFolder(MediaTree(project_).folderOf(*asset));
    mediaSelection = {id};
    folderSelection.clear();
    mediaAnchor = MediaPick::ofAsset(id);
    emit mediaSelectionChanged();
    emit mediaRevealRequested(MediaPick::ofAsset(id));
}

void EditorModel::dropDeletedFromDisk(const IdSet& gone) {
    if (gone.empty()) return;
    commitLive();
    changeLibrary([&](Project& p) { std::erase_if(p.assets, [&](const MediaAsset& a) { return gone.count(a.id) > 0; }); });
    droppedFromDisk_.insert(gone.begin(), gone.end());
    for (const auto& id : gone) mediaSelection.erase(id);
    if (mediaFolder) {
        const MediaTree tree(project_);
        std::optional<QString> up = mediaFolder;
        while (up && !tree.exists(*up)) up = tree.root(*up) ? std::optional<QString>(QFileInfo(*up).path()) : std::nullopt;
        if (up != mediaFolder) openMediaFolder(up);
    }
    emit mediaSelectionChanged();
}

void EditorModel::keepFolders(Project& p, const Project& now, const std::set<Uuid>& importing) {
    std::vector<MediaFolder> folders = now.folders.value_or(std::vector<MediaFolder>{});
    for (const auto& f : p.folders.value_or(std::vector<MediaFolder>{}))
        if (std::none_of(folders.begin(), folders.end(), [&](const MediaFolder& x) { return x.id == f.id; })) folders.push_back(f);
    if (folders.empty()) p.folders.reset(); else p.folders = folders;
    std::set<QString> hidden;
    for (const auto& h : p.hiddenMedia.value_or(std::vector<QString>{})) hidden.insert(h);
    for (const auto& h : now.hiddenMedia.value_or(std::vector<QString>{})) hidden.insert(h);
    if (hidden.empty()) p.hiddenMedia.reset(); else p.hiddenMedia = std::vector<QString>(hidden.begin(), hidden.end());
    p.pruneFolders(importing);
}

std::vector<MediaAsset> EditorModel::restoring(const std::vector<MediaAsset>& earlier, const std::vector<MediaAsset>& now) {
    std::map<Uuid, const MediaAsset*> current;
    for (const auto& a : now) current.emplace(a.id, &a);
    std::set<Uuid> earlierIDs;
    std::vector<MediaAsset> out;
    for (const auto& a : earlier) {
        earlierIDs.insert(a.id);
        auto it = current.find(a.id);
        out.push_back(it != current.end() ? *it->second : a);
    }
    for (const auto& a : now) if (!earlierIDs.count(a.id)) out.push_back(a);
    return out;
}

void EditorModel::redo() {
    commitLive();
    if (redoStack_.empty()) return;
    Project next = std::move(redoStack_.back());
    redoStack_.pop_back();
    std::set<Uuid> known;
    if (!redoKnown_.empty()) { known = redoKnown_.back(); redoKnown_.pop_back(); }
    undoStack_.push_back(project_);
    // Items the step deleted go again; everything else in the library stays as it is now.
    std::set<Uuid> deleted = known;
    for (const auto& a : next.assets) deleted.erase(a.id);
    std::vector<MediaAsset> assets;
    for (const auto& a : project_.assets) if (!deleted.count(a.id)) assets.push_back(a);
    next.assets = assets;
    keepFolders(next, project_, importingFolders_);
    next.magicHistory = project_.magicHistory;
    project_ = std::move(next);
    didChange();
}

void EditorModel::changeLibrary(const std::function<void(Project&)>& body, bool clearUndo) {
    commitLive();
    body(project_);
    if (clearUndo) { undoStack_.clear(); redoStack_.clear(); redoKnown_.clear(); }
    isDirty_ = true;
    lastEditMs_ = nowMs();
    didChange();
}

void EditorModel::didChange() {
    if (!captionSelection_.empty()) {
        std::set<QString> ids;
        for (const auto& b : captionBlocks()) ids.insert(b.id());
        std::set<QString> valid;
        for (const auto& s : captionSelection_) if (ids.count(s)) valid.insert(s);
        captionSelection_ = valid;
    }
    if (!project_.captions) captionsFocused_ = false;
    // Muting and soloing are heard at once, not after the rebuild.
    std::map<Uuid, bool> audible;
    for (const auto& t : project_.audioTracks) audible[t.id] = project_.isAudible(t);
    host_->setAudible(audible);
    IdSet valid;
    for (const auto& id : selection_) if (project_.clip(id)) valid.insert(id);
    if (valid != selection_) { selection_ = valid; emit selectionChanged(); }
    isDirty_ = true;
    // Adding or removing a video track moves the video/audio gap; keep it where it was on screen.
    const double above = rowsAbove();
    if (lastRowsAbove_ > 0 && above != lastRowsAbove_) setView(std::nullopt, std::nullopt, view_.scrollY + above - lastRowsAbove_);
    else clampView();
    setNeedsRebuild();
    emit changed();
}

// MARK: Zoom

void EditorModel::setViewSize(double width, double height) {
    if (width == view_.viewWidth && height == view_.viewHeight && !view_.needsInitialCentering) return;
    view_.viewWidth = width;
    view_.viewHeight = height;
    if (view_.needsInitialCentering && height > 0) {
        view_.needsInitialCentering = false;
        fitRows();
    }
    clampView();
}

double EditorModel::rowsContent() const {
    return TimelineLayout::fixedHeight + TimelineLayout::captionSpace(project_)
         + double(project_.videoTracks.size() + project_.audioTracks.size()) * (view_.rowHeight + TimelineLayout::gap);
}

double EditorModel::rowsAbove() const {
    return TimelineLayout::captionSpace(project_) + TimelineLayout::zone
         + double(project_.videoTracks.size()) * (view_.rowHeight + TimelineLayout::gap) + TimelineLayout::divider / 2;
}

double EditorModel::rowsWorld() const {
    const double zoomed = rowsVisible() * view_.rowHeight / TimelineViewState::rowMin;
    return std::max({rowsVisible(), zoomed, 2 * rowsAbove(), 2 * (rowsContent() - rowsAbove())});
}

double EditorModel::rowWindow(double r) const {
    const double tracks = double(project_.videoTracks.size() + project_.audioTracks.size());
    const double captions = TimelineLayout::captionSpace(project_);
    const double above = captions + TimelineLayout::zone + double(project_.videoTracks.size()) * (r + TimelineLayout::gap) + TimelineLayout::divider / 2;
    const double content = TimelineLayout::fixedHeight + captions + tracks * (r + TimelineLayout::gap);
    const double zoomed = rowsVisible() * r / TimelineViewState::rowMin;
    return rowsVisible() / std::max({rowsVisible(), zoomed, 2 * above, 2 * (content - above)});
}

std::pair<double, double> EditorModel::rowAnchor(double y) const {
    const double r = view_.rowHeight;
    std::vector<std::pair<double, bool>> segments = {{TimelineLayout::zone, false}};
    for (size_t i = 0; i < project_.videoTracks.size(); ++i) { segments.push_back({r, true}); segments.push_back({TimelineLayout::gap, false}); }
    segments.push_back({TimelineLayout::divider, false});
    for (size_t i = 0; i < project_.audioTracks.size(); ++i) { segments.push_back({r, true}); segments.push_back({TimelineLayout::gap, false}); }
    segments.push_back({TimelineLayout::zone, false});
    double fixed = std::min(0.0, y), rows = 0, pos = 0;
    for (const auto& [length, row] : segments) {
        const double take = std::min(std::max(y - pos, 0.0), length);
        if (row) rows += take / r; else fixed += take;
        pos += length;
    }
    fixed += std::max(0.0, y - pos);
    return {fixed, rows};
}

void EditorModel::fitRows() {
    // The track headers run on past the timeline, alongside the scroll bar under it: the add-audio zone, only
    // a label on the headers' side, can sit level with the bar.
    const double visible = rowsVisible() + RangeBarStyle::thickness;
    const double tracks = double(std::max<size_t>(1, project_.videoTracks.size() + project_.audioTracks.size()));
    const double room = visible - TimelineLayout::fixedHeight - TimelineLayout::captionSpace(project_) - 6;
    setView(std::nullopt, std::nullopt, std::nullopt, std::min(62.0, room / tracks - TimelineLayout::gap));
    if (rowsContent() <= visible) setView(std::nullopt, std::nullopt, (rowsContent() - visible) / 2);
    else setView(std::nullopt, std::nullopt, 0.0);
}

void EditorModel::setView(std::optional<double> pps, std::optional<double> scrollX, std::optional<double> scrollY, std::optional<double> rowHeight) {
    const TimelineViewState before = view_;
    if (rowHeight) view_.rowHeight = std::min(TimelineViewState::rowMax, std::max(TimelineViewState::rowMin, std::round(*rowHeight)));
    view_.pps = std::min(TimelineViewState::maxPPS, std::max(minPPS(), pps.value_or(view_.pps)));
    view_.scrollX = std::min(std::max(0.0, scrollX.value_or(view_.scrollX)), maxScrollX());
    view_.scrollY = std::min(std::max(minScrollY(), scrollY.value_or(view_.scrollY)), maxScrollY());
    lastRowsAbove_ = rowsAbove();
    if (before.pps != view_.pps || before.scrollX != view_.scrollX || before.scrollY != view_.scrollY || before.rowHeight != view_.rowHeight)
        emit viewChanged();
}

void EditorModel::setZoom(double newPPS) {
    const double old = view_.pps;
    const double pps = std::min(TimelineViewState::maxPPS, std::max(minPPS(), newPPS));
    if (std::abs(pps - old) <= 1e-9) return;
    const double px = playhead_ * old - view_.scrollX;
    const double anchor = (px >= 0 && px <= view_.viewWidth) ? px : view_.viewWidth / 2;
    setView(pps, playhead_ * pps - anchor);
}

void EditorModel::zoomToFit() {
    const double d = std::max(project_.duration(), 1.0);
    setView((view_.viewWidth - 80) / d, 0.0);
}

void EditorModel::setRebuildSuspended(bool s) {
    rebuildSuspended_ = s;
    if (!s) setNeedsRebuild();
}

void EditorModel::setNeedsRebuild() {
    if (rebuildPending_ || rebuildSuspended_) return;
    rebuildPending_ = true;
    const qint64 wait = std::max<qint64>(0, 30 - (nowMs() - lastRebuildMs_));
    QTimer::singleShot(int(wait), this, [this] { rebuildNow(); });
}

void EditorModel::flushRebuild() { rebuildNow(); }

void EditorModel::rebuildNow() {
    if (!rebuildPending_) return;
    rebuildPending_ = false;
    lastRebuildMs_ = nowMs();
    host_->rebuild(project_, playhead_);
    if (isPlaying_ && host_->engineEmpty()) { isPlaying_ = false; emit playingChanged(false); }
}

// MARK: Transport

Range EditorModel::loopSpan() const {
    if (auto r = project_.playRange()) return *r;
    return {0, project_.duration()};
}

void EditorModel::engineEnded() {
    if (loop_ && project_.duration() > 0) {
        const double start = loopSpan().start;
        playhead_ = start;
        host_->seek(start);
        host_->play();
        emit playheadChanged(playhead_);
        return;
    }
    isPlaying_ = false;
    playhead_ = project_.duration();
    emit playingChanged(false);
    emit playheadChanged(playhead_);
}

void EditorModel::tick() {
    if (!isPlaying_) return;
    playhead_ = std::min(host_->currentTime(), std::max(project_.duration(), 0.0));
    const Range span = loopSpan();
    if (loop_ && span.end > 0 && playhead_ >= span.end - 1 / project_.fps) {
        playhead_ = span.start;
        host_->seek(span.start);
        host_->play();
    }
    emit playheadChanged(playhead_);
}

void EditorModel::togglePlay() { isPlaying_ ? pause() : play(); }

void EditorModel::skim(std::optional<double> t) {
    if (!t) {
        if (skimTime_) {
            skimTime_.reset();
            if (!isPlaying_) host_->seek(playhead_);
        }
        return;
    }
    if (!skimming_ || isPlaying_) return;
    const double time = std::min(std::max(0.0, project_.snapFrame(*t)), project_.duration());
    if (skimTime_ == time) return;
    skimTime_ = time;
    if (!host_->showScrubFrame(time)) host_->seek(time, true);
    settleSoon();
    scrubSound(time);
}

void EditorModel::scrubSound(double t) {
    if (!audioScrub_ || isPlaying_) return;
    host_->scrubSound(t);
    lastScrubSound_ = {t, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()};
}

void EditorModel::play() {
    if (skimTime_) {
        if (boolSetting(playFromSkimmerKey, true)) playhead_ = *skimTime_;
        else host_->seek(playhead_);
    }
    skimTime_.reset();
    settleTimer_.stop();
    host_->endScrubFrames();
    if (!(project_.duration() > 0)) return;
    rebuildPending_ = true;
    rebuildNow();
    if (host_->engineEmpty()) return;
    const Range span = loopSpan();
    if (playhead_ >= project_.duration() - 1 / project_.fps ||
        (loop_ && project_.playRange() && (playhead_ < span.start || playhead_ >= span.end - 1 / project_.fps))) {
        playhead_ = span.start;
        host_->seek(span.start);
    }
    // Looping: the start is made ready to follow the end right away.
    host_->setLoopSpan(loop_ ? std::optional<Range>(span) : std::nullopt);
    host_->play();
    isPlaying_ = true;
    emit playingChanged(true);
    emit playheadChanged(playhead_);
}

void EditorModel::pause() {
    if (!isPlaying_) return;
    host_->pause();
    host_->setLoopSpan(std::nullopt);
    isPlaying_ = false;
    playhead_ = project_.snapFrame(host_->currentTime());
    host_->seek(playhead_);
    emit playingChanged(false);
    emit playheadChanged(playhead_);
}

void EditorModel::seek(double time) {
    skimTime_.reset();
    settleTimer_.stop();
    host_->endScrubFrames();
    if (isPlaying_) {
        host_->pause();
        isPlaying_ = false;
        emit playingChanged(false);
    }
    playhead_ = std::max(0.0, project_.snapFrame(time));
    host_->seek(playhead_);
    emit playheadChanged(playhead_);
}

void EditorModel::scrub(double time) {
    skimTime_.reset();
    if (isPlaying_) {
        host_->pause();
        isPlaying_ = false;
        emit playingChanged(false);
    }
    playhead_ = std::max(0.0, project_.snapFrame(time));
    // A frame from the made-ahead ones at once if they're there; the player's own seek otherwise.
    if (!host_->showScrubFrame(playhead_)) host_->seek(playhead_, true);
    settleSoon();
    emit playheadChanged(playhead_);
}

void EditorModel::settleSoon() { settleTimer_.start(); }

void EditorModel::step(int frames) { seek(playhead_ + double(frames) / project_.fps); }

void EditorModel::jumpToEdit(bool forward) {
    const auto points = project_.editPoints();
    const double eps = 0.5 / project_.fps;
    if (forward) {
        for (double t : points) if (t > playhead_ + eps) { seek(t); return; }
    } else {
        for (auto it = points.rbegin(); it != points.rend(); ++it) if (*it < playhead_ - eps) { seek(*it); return; }
    }
}

// MARK: Markers and in/out

void EditorModel::addMarker() {
    const double t = project_.snapFrame(playhead_);
    for (const auto& m : project_.markerList())
        if (std::abs(m.time - t) < 0.5 / project_.fps) { setEditingMarker(m.id); return; }
    Marker m;
    m.time = t;
    m.name = QStringLiteral("Marker %1").arg(project_.markers ? project_.markers->size() + 1 : 1);
    mutate([&](Project& p) {
        auto list = p.markers.value_or(std::vector<Marker>{});
        list.push_back(m);
        p.markers = list;
    });
}

static Marker* findMarker(Project& p, const Uuid& id) {
    if (!p.markers) return nullptr;
    for (auto& m : *p.markers) if (m.id == id) return &m;
    return nullptr;
}

void EditorModel::updateMarker(const Uuid& id, const std::function<void(Marker&)>& change) {
    mutate([&](Project& p) { if (auto* m = findMarker(p, id)) change(*m); });
}

void EditorModel::updateMarkerLive(const Uuid& id, const std::function<void(Marker&)>& change) {
    live([&](Project& p) { if (auto* m = findMarker(p, id)) change(*m); }, false);
}

void EditorModel::renameMarker(const Uuid& id, const QString& name) {
    live([&](Project& p) { if (auto* m = findMarker(p, id)) m->name = name; });
}

void EditorModel::setRangeEdgeLive(bool isIn, double t) {
    live([&](Project& p) { if (isIn) p.inPoint = t; else p.outPoint = t; }, false);
}

void EditorModel::deleteMarker(const Uuid& id) {
    if (editingMarker == id) setEditingMarker(std::nullopt);
    mutate([&](Project& p) {
        if (!p.markers) return;
        std::erase_if(*p.markers, [&](const Marker& m) { return m.id == id; });
        if (p.markers->empty()) p.markers.reset();
    });
}

void EditorModel::clearMarkers() {
    if (!project_.markers || project_.markers->empty()) return;
    setEditingMarker(std::nullopt);
    mutate([](Project& p) { p.markers.reset(); });
}

void EditorModel::setInPoint() {
    const double t = project_.snapFrame(playhead_);
    mutate([&](Project& p) {
        p.inPoint = t;
        if (p.outPoint && *p.outPoint <= t) p.outPoint.reset();
    });
}

void EditorModel::setOutPoint() {
    const double t = project_.snapFrame(playhead_);
    mutate([&](Project& p) {
        p.outPoint = t;
        if (p.inPoint && *p.inPoint >= t) p.inPoint.reset();
    });
}

void EditorModel::clearRange() {
    if (!project_.inPoint && !project_.outPoint) return;
    mutate([](Project& p) { p.inPoint.reset(); p.outPoint.reset(); });
}

// MARK: Adding clips

double EditorModel::payloadDuration(const DragPayload& payload) const {
    if (payload.kind == DragPayload::Kind::asset) {
        const MediaAsset* a = project_.asset(payload.asset);
        if (a && a->kind != MediaKind::image)
            return std::max(1 / project_.fps, std::floor(a->duration * project_.fps + 1e-6) / project_.fps);
    }
    return 5;
}

std::vector<Clip> EditorModel::makeClips(const DragPayload& payload, double start, const Uuid& videoTrack, const Uuid& audioTrack) const {
    const double dur = payloadDuration(payload);
    switch (payload.kind) {
    case DragPayload::Kind::solid: return {Clip(videoTrack, ClipKind::solid, start, dur)};
    case DragPayload::Kind::text: return {Clip(videoTrack, ClipKind::text, start, dur)};
    case DragPayload::Kind::asset: {
        const MediaAsset* a = project_.asset(payload.asset);
        if (!a) return {};
        switch (a->kind) {
        case MediaKind::image: { Clip c(videoTrack, ClipKind::image, start, dur); c.assetID = a->id; return {c}; }
        case MediaKind::audio: { Clip c(audioTrack, ClipKind::audio, start, dur); c.assetID = a->id; return {c}; }
        case MediaKind::video: {
            Clip v(videoTrack, ClipKind::video, start, dur);
            v.assetID = a->id;
            if (!a->hasAudio) return {v};
            const Uuid link = newUuid();
            v.linkID = link;
            Clip au(audioTrack, ClipKind::audio, start, dur);
            au.assetID = a->id;
            au.linkID = link;
            return {v, au};
        }
        }
    }
    }
    return {};
}

void EditorModel::insert(const std::vector<Clip>& clips) {
    if (clips.empty()) return;
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    mutate([&](Project& p) {
        p.clips.insert(p.clips.end(), clips.begin(), clips.end());
        p.materializeTokens();
        p.resolveOverlaps(ids);
    });
    setSelection(ids);
}

void EditorModel::addAtPlayhead(const DragPayload& payload) {
    const double dur = payloadDuration(payload);
    // Text and solids never cover anything: the first open video track at the playhead, or else
    // the first gap after it.
    if (payload.kind != DragPayload::Kind::asset) {
        if (auto spot = project_.firstFreeVideoSpot(playhead_, dur)) {
            const Uuid audio = project_.audioTracks.empty() ? TrackToken::newAudio() : project_.audioTracks.front().id;
            insert(makeClips(payload, spot->second, spot->first, audio));
        } else {
            insert(makeClips(payload, playhead_, TrackToken::newVideo(), TrackToken::newAudio()));
        }
        return;
    }
    const double start = playhead_;
    const Uuid v = project_.freeTrack(TrackKind::video, start, dur).value_or(TrackToken::newVideo());
    const Uuid a = project_.freeTrack(TrackKind::audio, start, dur).value_or(TrackToken::newAudio());
    insert(makeClips(payload, start, v, a));
}

const std::vector<double>& EditorModel::frameRates() {
    static const std::vector<double> rates{23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 100, 119.88, 120};
    return rates;
}

double EditorModel::standardFPS(double fps) {
    if (!(fps > 1)) return 30;
    const auto& r = frameRates();
    return *std::min_element(r.begin(), r.end(), [&](double a, double b) { return std::abs(a - fps) < std::abs(b - fps); });
}

// MARK: Editing commands

void EditorModel::deleteSelection(bool ripple) {
    if (selection_.empty() && !captionSelection_.empty()) return deleteSelectedCaptions();
    if (selection_.empty()) return;
    const IdSet ids = project_.unlocked(project_.linked(selection_));
    if (ids.empty()) return;
    mutate([&](Project& p) { p.remove(ids, ripple); });
}

void EditorModel::deleteSelected(bool ripple) {
    if (!mediaSelection.empty() || !folderSelection.empty()) removeAssets(mediaSelection, folderSelection);
    else deleteSelection(ripple);
}

void EditorModel::removeAssets(const IdSet& picked, const std::set<QString>& folders) {
    const MediaTree tree(project_);
    IdSet inFolders;
    for (const auto& f : folders) for (const auto& a : tree.assetsUnder(f, project_.assets)) inFolders.insert(a.id);
    // Items picked on their own (not in a folder that's going).
    IdSet loose;
    for (const auto& id : picked) if (!inFolders.count(id)) loose.insert(id);
    IdSet ids = loose;
    ids.insert(inFolders.begin(), inFolders.end());
    if (ids.empty()) {
        // Folders with nothing in them (missing ones, say): just let go.
        if (folders.empty()) return;
        changeLibrary([&](Project& p) {
            if (p.folders) std::erase_if(*p.folders, [&](const MediaFolder& f) { return folders.count(f.path) > 0; });
            p.pruneFolders(importingFolders_);
        });
        for (const auto& f : folders) folderSelection.erase(f);
        return;
    }
    std::set<Uuid> groups;
    bool used = false;
    for (const auto& c : project_.clips)
        if (c.assetID && ids.count(*c.assetID)) { used = true; groups.insert(c.linkID.value_or(c.id)); }
    // A video and its linked sound count as one clip, as they look on the timeline.
    const int clipCount = int(groups.size());
    if (used) {
        QStringList names;
        for (const auto& a : project_.assets) if (loose.count(a.id)) names << a.name;
        for (const auto& f : folders) names << MediaTree::name(f);
        const QString message = names.size() == 1
            ? (folders.empty() ? QStringLiteral("Delete “%1”?").arg(names[0]) : QStringLiteral("Delete the folder “%1”?").arg(names[0]))
            : QStringLiteral("Delete %1 items?").arg(names.size());
        const QString info = QStringLiteral("%1 clip%2 %3 and will be deleted too. The files stay on your computer, and Undo brings everything back.")
            .arg(clipCount).arg(clipCount == 1 ? QStringLiteral(" on the timeline uses") : QStringLiteral("s on the timeline use"))
            .arg(names.size() == 1 ? QStringLiteral("it") : QStringLiteral("them"));
        if (host_->alert(message, info, {QStringLiteral("Delete"), QStringLiteral("Cancel")}) != 0) return;
    }
    const auto importing = importingFolders_;
    mutate([&](Project& p) {
        // Files in a linked folder are left out of it from now on, so they don't come back.
        std::vector<QString> hidden = p.hiddenMedia.value_or(std::vector<QString>{});
        for (const auto& folder : folders) {
            const bool linked = p.folders && std::any_of(p.folders->begin(), p.folders->end(), [&](const MediaFolder& f) { return f.path == folder; });
            if (linked) std::erase_if(*p.folders, [&](const MediaFolder& f) { return f.path == folder; });
            else hidden.push_back(folder);
        }
        for (const auto& a : p.assets) if (loose.count(a.id) && p.linkedFolder(a.path)) hidden.push_back(a.path);
        if (hidden.empty()) p.hiddenMedia.reset(); else p.hiddenMedia = hidden;
        std::erase_if(p.clips, [&](const Clip& c) { return c.assetID && ids.count(*c.assetID); });
        std::erase_if(p.assets, [&](const MediaAsset& a) { return ids.count(a.id) > 0; });
        p.pruneFolders(importing);
    });
    for (const auto& id : ids) mediaSelection.erase(id);
    std::erase_if(folderSelection, [&](const QString& f) {
        return std::any_of(folders.begin(), folders.end(), [&](const QString& g) { return MediaFolders::contains(g, f); });
    });
    if (mediaFolder && !MediaTree(project_).exists(*mediaFolder)) {
        // Back up to the nearest folder that's still there.
        const MediaTree t(project_);
        std::optional<QString> up = mediaFolder;
        while (up && !t.exists(*up)) up = t.root(*up) ? std::optional<QString>(QFileInfo(*up).path()) : std::nullopt;
        mediaFolder = up;
    }
    IdSet sel;
    for (const auto& id : selection_) if (project_.clip(id)) sel.insert(id);
    setSelection(sel);
}

void EditorModel::splitAtPlayhead() {
    const double t = playhead_;
    IdSet under, selected;
    for (const auto& c : project_.clips) if (c.start < t && c.end() > t) { under.insert(c.id); if (selection_.count(c.id)) selected.insert(c.id); }
    const IdSet targets = project_.unlocked(project_.linked(selected.empty() ? under : selected));
    if (targets.empty()) return;
    mutate([&](Project& p) { p.split(targets, t); });
}

void EditorModel::rippleTrim(bool head) {
    // While playing, trim where it's playing now and carry on playing afterwards.
    const bool wasPlaying = isPlaying_;
    pause();
    auto range = rippleTrimRange(head);
    if (!range) { if (wasPlaying) play(); return; }
    auto [a, b, tracks] = *range;
    mutate([&](Project& p) { p.rippleRemove(a, b, tracks); });
    if (head) seek(a);
    if (wasPlaying) play();
}

bool EditorModel::canRippleTrim() const { return rippleTrimRange(true) || rippleTrimRange(false); }

std::optional<std::tuple<double, double, IdSet>> EditorModel::rippleTrimRange(bool head) const {
    const double t = playhead_;
    const Project& p = project_;
    std::vector<const Clip*> under, selected;
    for (const auto& c : p.clips)
        if (c.start < t && c.end() > t && !p.isLocked(c.trackID)) { under.push_back(&c); if (selection_.count(c.id)) selected.push_back(&c); }
    IdSet tracks;
    if (selected.empty()) {
        for (const auto& tr : p.videoTracks) tracks.insert(tr.id);
        for (const auto& tr : p.audioTracks) tracks.insert(tr.id);
    } else {
        IdSet s;
        for (auto* c : selected) s.insert(c->id);
        const IdSet ids = p.linked(s);
        for (const auto& c : p.clips) if (ids.count(c.id)) tracks.insert(c.trackID);
    }
    std::vector<const Clip*> edges;
    for (auto* c : under) if (tracks.count(c->trackID)) edges.push_back(c);
    if (edges.empty()) return std::nullopt;
    // The nearest edit before (or after) the playhead on those tracks.
    double lo, hi;
    if (head) {
        lo = edges[0]->start;
        for (auto* c : edges) lo = std::max(lo, c->start);
        hi = t;
    } else {
        lo = t;
        hi = edges[0]->end();
        for (auto* c : edges) hi = std::min(hi, c->end());
    }
    if (!(hi - lo >= 0.5 / p.fps)) return std::nullopt;
    IdSet open;
    for (const auto& tr : tracks) if (!p.isLocked(tr)) open.insert(tr);
    return std::make_tuple(lo, hi, open);
}

void EditorModel::toggleZoom(const Uuid& id) {
    const Clip* c = project_.clip(id);
    if (!c || !c->isTransformable()) return;
    if (c->transform && c->transform->zoom) {
        mutate([&](Project& p) {
            auto i = p.index(id);
            if (!i) return;
            p.clips[*i].transform->zoom.reset();
            if (p.clips[*i].transform->isIdentity()) p.clips[*i].transform.reset();
        });
        return;
    }
    QSettings s;
    const QString raw = s.value(QLatin1String(ZoomMove::modeKey)).toString();
    const ZoomMove::Mode mode = raw == QStringLiteral("out") ? ZoomMove::Mode::zoomOut
                              : raw == QStringLiteral("inOut") ? ZoomMove::Mode::inOut : ZoomMove::Mode::zoomIn;
    mutate([&](Project& p) {
        auto i = p.index(id);
        if (!i) return;
        ClipTransform t = p.clips[*i].transform.value_or(ClipTransform());
        if (t.scale <= 1.01) t.scale = 2;
        t.zoom = ZoomMove::starter(mode, p.clips[*i].duration);
        p.clips[*i].transform = t;
    });
    showZoomed(id);
}

void EditorModel::showZoomed(const Uuid& id) {
    const Clip* c = project_.clip(id);
    if (isPlaying_ || !c || !c->transform || !c->transform->zoom) return;
    const ZoomMove& z = *c->transform->zoom;
    const double u = playhead_ - c->start;
    if (u >= 0 && u < c->duration && z.amount(u) > 0.999) return;
    seek(c->start + z.zoomedTime(c->duration));
}

void EditorModel::split(const Uuid& id, double t) {
    const IdSet targets = project_.linked({id});
    mutate([&](Project& p) { p.split(targets, t); });
}

void EditorModel::detachAudio() {
    const IdSet ids = selection_;
    mutate([&](Project& p) { p.unlink(ids); });
}

bool EditorModel::selectionIsLinked() const {
    return std::any_of(project_.clips.begin(), project_.clips.end(), [&](const Clip& c) { return selection_.count(c.id) && c.linkID; });
}

void EditorModel::selectAll() {
    IdSet all;
    for (const auto& c : project_.clips) all.insert(c.id);
    setSelection(all);
    // Even when every clip was already picked (so the selection didn't change): Media lets go.
    clearMediaPicks();
}

// MARK: Captions

std::vector<CaptionBlock> EditorModel::captionBlocks() const {
    if (!project_.captions) return {};
    // The cache key: the captioned clips, the fixes and the transcripts' version.
    Project keyed;
    keyed.videoTracks.clear();
    keyed.audioTracks.clear();
    keyed.clips = project_.captionedClips();
    keyed.captionFixes = project_.captionFixes;
    QByteArray key = codec::compactBytes(SavedDocument{1, keyed, {}});
    key += QByteArray::number(transcriptVersion_);
    if (captionCache_ && captionCache_->first == key) return captionCache_->second;
    auto blocks = CaptionLayout::blocks(project_, [this](const MediaAsset& a) { return host_->transcript(a.id); });
    captionCache_ = std::make_pair(key, blocks);
    return blocks;
}

bool EditorModel::captionsWaiting() const {
    for (const auto& c : project_.captionedClips()) if (c.assetID && host_->transcriptPending(*c.assetID)) return true;
    return false;
}

void EditorModel::transcriptsChanged() {
    if (!project_.captions) return;
    ++transcriptVersion_;
    setNeedsRebuild();
    emit changed();
}

int EditorModel::addCaptions(const std::optional<IdSet>& idsIn) {
    const IdSet picked = idsIn.value_or(selection_);
    // Everything picked (Ctrl+A), or nothing: everyone talking, not the music.
    bool everything = picked.empty();
    if (!everything) {
        everything = std::all_of(project_.clips.begin(), project_.clips.end(), [&](const Clip& c) { return picked.count(c.id) > 0; });
    }
    const IdSet targets = project_.unlocked(everything ? project_.defaultCaptionTargets() : project_.captionTargets(picked));
    if (targets.empty()) return 0;
    // No speech model yet: offered (only downloaded when the person says so).
    if (!host_->transcriptsAvailable() && !host_->offerSpeechModel()) return -1;
    for (const auto& c : project_.clips) if (targets.count(c.id) && c.assetID) host_->makeTranscript(*c.assetID, true);
    mutate([&](Project& p) { p.addCaptions(targets); });
    return int(targets.size());
}

QString EditorModel::captionsMenuTitle() const {
    return !selection_.empty() && hasCaptions(selection_) ? QStringLiteral("Remove Captions") : QStringLiteral("Add Captions");
}

void EditorModel::captionsFromMenu() {
    if (!selection_.empty() && hasCaptions(selection_)) return removeCaptions();
    if (addCaptions() != 0) return;   // (-1: the speech model was offered and declined)
    host_->alert(QStringLiteral("Nothing to caption"),
                 selection_.empty()
                     ? QStringLiteral("Captions come from what's said in clips of people talking. Add some to the timeline, or pick a clip with sound.")
                     : QStringLiteral("The picked clips have no sound to caption."),
                 {QStringLiteral("OK")});
}

void EditorModel::removeCaptions(const std::optional<IdSet>& ids) {
    const IdSet targets = project_.captionTargets(ids.value_or(selection_));
    if (targets.empty()) return;
    mutate([&](Project& p) { p.removeCaptions(targets); });
}

void EditorModel::removeCaptionTrack() {
    captionSelection_.clear();
    mutate([](Project& p) { p.removeCaptionTrack(); });
}

bool EditorModel::hasCaptions(const IdSet& ids) const {
    const IdSet targets = project_.captionTargets(ids);
    return std::any_of(project_.clips.begin(), project_.clips.end(), [&](const Clip& c) { return targets.count(c.id) && c.captioned == true; });
}

void EditorModel::selectCaptions(const std::set<QString>& ids) {
    if (!selection_.empty()) { selection_.clear(); emit selectionChanged(); }
    captionSelection_ = ids;
    captionsFocused_ = false;
    emit changed();
}

void EditorModel::releaseCaptions() {
    captionSelection_.clear();
    captionsFocused_ = false;
    emit changed();
}

void EditorModel::focusCaptions() {
    if (!selection_.empty()) { selection_.clear(); emit selectionChanged(); }
    captionSelection_.clear();
    captionsFocused_ = true;
    emit changed();
}

void EditorModel::deleteSelectedCaptions() {
    std::vector<CaptionBlock> picked;
    for (const auto& b : captionBlocks()) if (captionSelection_.count(b.id())) picked.push_back(b);
    if (picked.empty()) return;
    captionSelection_.clear();
    mutate([&](Project& p) { for (const auto& b : picked) p.setCaptionText(b, QString()); });
}

void EditorModel::setCaptionText(const CaptionBlock& block, const QString& text) {
    mutate([&](Project& p) { p.setCaptionText(block, text); });
}

void EditorModel::typeCaptionText(const CaptionBlock& block, const QString& text) {
    QString t = text;
    t.remove(u' ');
    t.remove(u'\t');
    if (t.isEmpty()) return;
    live([&](Project& p) { p.setCaptionText(block, text); }, false);
}

void EditorModel::setCaptionsHidden(bool hidden) {
    mutate([&](Project& p) { if (p.captions) p.captions->hidden = hidden; });
}

void EditorModel::setCaptionStyle(const std::function<void(CaptionStyle&)>& change, bool isLive, bool settles) {
    if (!project_.captions) return;
    if (isLive) live([&](Project& p) { if (p.captions) change(p.captions->style); }, settles);
    else mutate([&](Project& p) { if (p.captions) change(p.captions->style); });
}

// MARK: Freeze frame

std::optional<Clip> EditorModel::freezeTarget() const {
    const double t = project_.snapFrame(playhead_);
    for (const auto& c : project_.clips)
        if (selection_.count(c.id) && c.kind == ClipKind::video && !c.isFreeze() && !project_.isLocked(c.trackID) && t >= c.start && t < c.end())
            return c;
    return std::nullopt;
}

void EditorModel::freezeFrame(double hold) {
    const double t = project_.snapFrame(playhead_);
    auto target = freezeTarget();
    if (!target) return;
    const Clip c = *target;
    double source = c.sourceTime(t);
    if (c.playsBackwards()) if (const MediaAsset* a = project_.asset(c.assetID)) source = std::max(0.0, a->duration - source - 0.001);
    std::optional<Uuid> made;
    mutate([&](Project& p) {
        const IdSet group = p.linked({c.id});
        p.split(group, t);
        std::set<Uuid> tracks;
        for (const auto& x : p.clips) if (group.count(x.id)) tracks.insert(x.trackID);
        IdSet shifting;
        for (const auto& x : p.clips) if (tracks.count(x.trackID) && x.start >= t - 1e-6) shifting.insert(x.id);
        shifting = p.unlocked(p.linked(shifting));
        for (auto& x : p.clips) if (shifting.count(x.id)) x.start += hold;
        Clip f(c.trackID, ClipKind::video, t, hold, source);
        f.assetID = c.assetID;
        f.freeze = true;
        f.grade = c.grade;
        f.transform = c.transform;
        p.clips.push_back(f);
        IdSet keep = shifting;
        keep.insert(f.id);
        p.resolveOverlaps(keep);
        made = f.id;
    });
    if (made) setSelection({*made});
}

// MARK: Reverse

void EditorModel::reverseSelection() {
    const Clip* found = nullptr;
    for (const auto& c : project_.clips)
        if (selection_.count(c.id) && c.playsSource() && !project_.isLocked(c.trackID)) { found = &c; break; }
    if (!found) return;
    const Clip c = *found;
    const IdSet group = project_.unlocked(project_.linked({c.id}));

    // Older projects reversed into a copy of the file: point back at the original footage.
    if (c.reversedFrom) {
        mutate([&](Project& p) {
            for (auto& x : p.clips) {
                if (!group.count(x.id) || !x.reversedFrom) continue;
                x.assetID = x.reversedFrom->assetID;
                x.sourceStart = x.reversedFrom->sourceStart;
                x.reversedFrom.reset();
            }
        });
        return;
    }
    const bool backwards = !c.playsBackwards();
    if (backwards)
        for (const auto& m : project_.clips) if (group.count(m.id) && m.kind == ClipKind::audio && m.assetID) host_->prepareReversedAudio(*m.assetID);
    mutate([&](Project& p) {
        for (auto& x : p.clips) {
            if (!group.count(x.id) || !x.playsSource() || x.reversedFrom) continue;
            const MediaAsset* a = p.asset(x.assetID);
            if (!a) continue;
            const double length = a->duration;
            const Clip m = x;
            // The same stretch of the file, measured from the other end.
            x.sourceStart = std::max(0.0, length - (m.sourceStart + m.sourceLength()));
            x.ramp = m.ramp ? std::optional<SpeedRamp>(m.ramp->mirrored(length)) : std::nullopt;
            x.reversed = backwards ? std::optional<bool>(true) : std::nullopt;
        }
    });
}

// MARK: Gaps

void EditorModel::deleteGap(const Uuid& track, double t) {
    auto g = project_.gap(track, t);
    if (!g) return;
    mutate([&](Project& p) { p.pullLeft(track, g->end, g->end - g->start); });
}

void EditorModel::closeAllGaps(const std::optional<std::vector<Uuid>>& tracks) {
    std::vector<Uuid> chosen;
    if (tracks) chosen = *tracks;
    else {
        std::set<Uuid> selected;
        for (const auto& c : project_.clips) if (selection_.count(c.id)) selected.insert(c.trackID);
        if (selected.empty()) {
            for (const auto& t : project_.videoTracks) chosen.push_back(t.id);
            for (const auto& t : project_.audioTracks) chosen.push_back(t.id);
        } else {
            chosen.assign(selected.begin(), selected.end());
        }
    }
    mutate([&](Project& p) { for (const auto& t : chosen) p.closeAllGaps(t); });
}

// MARK: Copy, paste, grade

std::vector<Clip> EditorModel::gradableSelection() const {
    const IdSet ids = project_.unlocked(selection_);
    std::vector<Clip> out;
    for (const auto& c : project_.clips) if (ids.count(c.id) && c.kind != ClipKind::audio) out.push_back(c);
    return out;
}

bool EditorModel::canCopyGrade() const { return gradableSelection().size() == 1; }
bool EditorModel::canPasteGrade() const { return copiedGrade_.has_value() && !gradableSelection().empty(); }

void EditorModel::copyGrade() {
    const auto g = gradableSelection();
    if (g.empty()) return;
    copiedGrade_ = g.front().grade;
    emit changed();
}

void EditorModel::pasteGrade() {
    if (!copiedGrade_) return;
    IdSet ids;
    for (const auto& c : gradableSelection()) ids.insert(c.id);
    if (ids.empty()) return;
    const auto copied = *copiedGrade_;
    mutate([&](Project& p) { for (auto& c : p.clips) if (ids.count(c.id)) c.grade = copied; });
}

std::vector<Clip> EditorModel::selectedForCopy() const {
    const IdSet ids = project_.unlocked(project_.linked(selection_));
    std::vector<Clip> out;
    for (const auto& c : project_.clips) if (ids.count(c.id)) out.push_back(c);
    return out;
}

void EditorModel::copySelection() {
    auto clips = selectedForCopy();
    if (clips.empty()) return;
    double t0 = clips[0].start;
    for (const auto& c : clips) t0 = std::min(t0, c.start);
    std::set<Uuid> used;
    Clipboard board;
    for (const auto& c : clips) {
        if (c.assetID) used.insert(*c.assetID);
        if (c.reversedFrom) used.insert(c.reversedFrom->assetID);
        if (auto i = project_.trackIndex(c.trackID, trackKindOf(c.kind))) board.trackIndex[c.id] = *i;
    }
    for (auto& c : clips) c.start -= t0;
    board.clips = clips;
    for (const auto& a : project_.assets) if (used.count(a.id)) board.assets.push_back(a);
    board.source = serial_;
    sharedClipboard() = board;
}

void EditorModel::cutSelection() {
    copySelection();
    deleteSelection();
}

void EditorModel::paste() {
    const Clipboard board = sharedClipboard();
    if (board.clips.empty()) return;
    if (board.source == serial_) {
        const auto copies = placed(board.clips, playhead_);
        insert(copies);
        double end = copies[0].end();
        for (const auto& c : copies) end = std::max(end, c.end());
        seek(end);
        return;
    }
    // From another project: its media is found here by file, or added to Media (the same files on disk), and its clips
    // go on the tracks in the same places.
    commitLive();
    std::map<Uuid, Uuid> map;
    std::vector<MediaAsset> added;
    for (const auto& a : board.assets) {
        const MediaAsset* here = nullptr;
        for (const auto& x : project_.assets) if (x.id == a.id || x.path == a.path) { here = &x; break; }
        if (here) map[a.id] = here->id;
        else { added.push_back(a); map[a.id] = a.id; }
    }
    std::vector<Clip> clips;
    for (const Clip& original : board.clips) {
        Clip c = original;
        if (c.assetID) if (auto it = map.find(*c.assetID); it != map.end()) c.assetID = it->second;
        if (c.reversedFrom) if (auto it = map.find(c.reversedFrom->assetID); it != map.end()) c.reversedFrom->assetID = it->second;
        const TrackKind kind = trackKindOf(c.kind);
        const auto& tracks = project_.tracks(kind);
        auto i = board.trackIndex.find(original.id);
        if (i != board.trackIndex.end() && i->second < tracks.size() && !tracks[i->second].locked) c.trackID = tracks[i->second].id;
        else c.trackID = kind == TrackKind::video ? TrackToken::newVideo() : TrackToken::newAudio();
        clips.push_back(c);
    }
    const auto copies = placed(clips, playhead_);
    IdSet ids;
    for (const auto& c : copies) ids.insert(c.id);
    if (!added.empty()) {
        // The library isn't undone: the media comes in first, then the clips as one step.
        changeLibrary([&](Project& p) { p.assets.insert(p.assets.end(), added.begin(), added.end()); });
        host_->expect(added);
    }
    mutate([&](Project& p) {
        p.clips.insert(p.clips.end(), copies.begin(), copies.end());
        p.materializeTokens();
        p.resolveOverlaps(ids);
    });
    setSelection(ids);
    double end = copies[0].end();
    for (const auto& c : copies) end = std::max(end, c.end());
    seek(end);
    if (!added.empty()) setNeedsRebuild();
}

void EditorModel::duplicateSelection() {
    auto clips = selectedForCopy();
    if (clips.empty()) return;
    double t0 = clips[0].start, t1 = clips[0].end();
    for (const auto& c : clips) { t0 = std::min(t0, c.start); t1 = std::max(t1, c.end()); }
    for (auto& c : clips) c.start -= t0;
    insert(placed(clips, t1));
}

std::vector<Clip> EditorModel::placed(const std::vector<Clip>& clips, double t) const {
    std::map<Uuid, Uuid> links;
    std::vector<Clip> out;
    for (Clip c : clips) {
        c.id = newUuid();
        if (c.linkID) {
            if (!links.count(*c.linkID)) links[*c.linkID] = newUuid();
            c.linkID = links[*c.linkID];
        }
        c.start += t;
        const bool token = c.trackID == TrackToken::newVideo() || c.trackID == TrackToken::newAudio();
        if (!token && (!project_.track(c.trackID) || project_.isLocked(c.trackID))) {
            const TrackKind kind = trackKindOf(c.kind);
            std::optional<Uuid> open;
            for (const auto& tr : project_.tracks(kind)) if (!tr.locked) { open = tr.id; break; }
            c.trackID = open.value_or(kind == TrackKind::video ? TrackToken::newVideo() : TrackToken::newAudio());
        }
        out.push_back(c);
    }
    return out;
}

void EditorModel::evenOutLevels() {
    std::vector<Clip> chosen;
    for (const auto& c : project_.clips) if (selection_.count(c.id) && c.kind == ClipKind::audio) chosen.push_back(c);
    if (chosen.empty()) for (const auto& c : project_.clips) if (c.kind == ClipKind::audio) chosen.push_back(c);
    std::erase_if(chosen, [&](const Clip& c) { return project_.isLocked(c.trackID); });
    const auto gains = levelGains(chosen);
    if (gains.empty()) return;
    mutate([&](Project& p) {
        for (auto& c : p.clips) if (auto it = gains.find(c.id); it != gains.end()) c.gainDB = std::clamp(it->second, -24.0, 12.0);
    });
}

std::map<Uuid, double> EditorModel::levelGains(const std::vector<Clip>& clips, bool matchOutput) const {
    struct M { Clip clip; double level, peak, track; };
    std::vector<M> measured;
    for (const auto& c : clips) {
        auto level = host_->envelopeDB(c);
        if (!level) continue;
        const Track* t = project_.track(c.trackID);
        const double track = matchOutput ? (t ? t->gainDB : 0) : 0;
        measured.push_back({c, *level, host_->peakDB(c).value_or(*level), track});
    }
    if (measured.empty()) return {};
    double target;
    if (matchOutput) {
        std::vector<double> outputs;
        for (const auto& m : measured) outputs.push_back(m.level + m.clip.gainDB + m.track);
        std::sort(outputs.begin(), outputs.end());
        const size_t n = outputs.size();
        target = n % 2 == 1 ? outputs[n / 2] : (outputs[n / 2 - 1] + outputs[n / 2]) / 2;
    } else {
        target = -20;
    }
    std::map<Uuid, double> gains;
    for (const auto& m : measured) {
        double g = target - m.level - m.track;
        if (matchOutput) g = std::min(g, -1 - m.peak - m.track);
        gains[m.clip.id] = std::clamp(std::round(g * 10) / 10, -24.0, 12.0);
    }
    return gains;
}

void EditorModel::duck(const Uuid& track, const std::optional<Uuid>& voice, std::optional<double> amount) {
    mutate([&](Project& p) {
        p.updateTrack(track, [&](Track& t) {
            t.duckUnder = voice;
            if (amount) t.duckDB = *amount;
        });
    });
}

void EditorModel::setTransition(const Uuid& into, const std::optional<Transition>& t) {
    mutate([&](Project& p) { if (auto i = p.index(into)) p.clips[*i].transitionIn = t; });
}

std::optional<Clip> EditorModel::dissolveTarget() const {
    std::optional<Clip> best;
    for (const auto& c : project_.clips) {
        if (c.kind == ClipKind::audio || project_.isLocked(c.trackID)) continue;
        const Clip* prev = project_.previousTouching(c);
        if (!prev) continue;
        if (!(selection_.count(c.id) || selection_.count(prev->id))) continue;
        if (!best || std::abs(c.start - playhead_) < std::abs(best->start - playhead_)) best = c;
    }
    return best;
}

void EditorModel::addTransitionAtPlayhead() {
    auto best = dissolveTarget();
    if (!best) return;
    setTransition(best->id, best->transitionIn.value_or(Transition()));
}

bool EditorModel::canReverse() const {
    return std::any_of(project_.clips.begin(), project_.clips.end(), [&](const Clip& c) {
        return selection_.count(c.id) && c.playsSource() && !project_.isLocked(c.trackID);
    });
}

bool EditorModel::canDeleteSelected() const {
    return !selection_.empty() || !mediaSelection.empty() || !folderSelection.empty() || !captionSelection_.empty();
}

bool EditorModel::canCaption() const { return !project_.captionTargets(selection_).empty() || selection_.empty(); }

void EditorModel::resetPlayback(const IdSet& volume) {
    const IdSet ids = project_.linked(selection_);
    const bool st = sticky_;
    mutate([&](Project& p) {
        p.retime(ids, st, [](Clip& c) { c.speed = 1; c.ramp.reset(); });
        for (auto& c : p.clips) if (volume.count(c.id)) c.gainDB = 0;
    });
}

void EditorModel::setSpeed(double speed) {
    const IdSet ids = project_.linked(selection_);
    const bool st = sticky_;
    mutate([&](Project& p) { p.setSpeed(ids, speed, st); });
}

void EditorModel::addTrack(TrackKind kind) { mutate([&](Project& p) { p.addTrack(kind); }); }
void EditorModel::removeTrack(const Uuid& id) { mutate([&](Project& p) { p.removeTrack(id); }); }

void EditorModel::setSequence(int width, int height) {
    mutate([&](Project& p) { p.width = width; p.height = height; p.settingsLocked = true; });
}

const MediaAsset* EditorModel::matchableAsset() const {
    for (const auto& c : project_.clips) {
        if (!selection_.count(c.id) || c.kind != ClipKind::video) continue;
        const MediaAsset* a = project_.asset(c.assetID);
        return a && a->width > 0 ? a : nullptr;
    }
    return nullptr;
}

bool EditorModel::sequenceMatches(const MediaAsset& a) const {
    return a.width == project_.width && a.height == project_.height && standardFPS(a.fps) == project_.fps;
}

void EditorModel::matchSequence(const MediaAsset& a) {
    if (a.kind != MediaKind::video || a.width <= 0 || a.height <= 0) return;
    // Frame size and rate only: HDR is picked by hand (Dynamic range), never by matching an iPhone clip.
    mutate([&](Project& p) { p.width = a.width; p.height = a.height; p.fps = standardFPS(a.fps); p.settingsLocked = true; });
}

void EditorModel::setHDR(bool on) {
    if (project_.isHDR() == on) return;
    mutate([&](Project& p) { p.hdr = on ? std::optional<bool>(true) : std::nullopt; p.settingsLocked = true; });
}

void EditorModel::setFPS(double fps) {
    mutate([&](Project& p) { p.fps = fps; p.settingsLocked = true; });
}

void EditorModel::setExportTarget(const QString& name, const QString& directory) {
    if (project_.exportName == name && project_.exportDir == directory) return;
    project_.exportName = name;
    project_.exportDir = directory;
    isDirty_ = true;
    emit changed();
}

const std::vector<MagicHistoryEntry>& EditorModel::magicHistory() const {
    return project_.magicHistory ? *project_.magicHistory : emptyHistory_;
}

void EditorModel::addMagicPrompt(const QString& text, const QString& tab) {
    // MagicHistory.adding: the last 20, newest first, each once.
    const QString t = text.trimmed();
    if (t.isEmpty()) return;
    std::vector<MagicHistoryEntry> list = magicHistory();
    std::erase_if(list, [&](const MagicHistoryEntry& e) { return e.text == t; });
    MagicHistoryEntry e;
    e.text = t;
    e.tab = tab;
    // Swift's Date: seconds since 2001-01-01 UTC.
    e.date = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0 - 978307200.0;
    list.insert(list.begin(), e);
    if (list.size() > 20) list.resize(20);
    project_.magicHistory = list;
    isDirty_ = true;
    emit changed();
}

// MARK: Files

const QStringList& EditorModel::projectExtensions() {
    static const QStringList e{QStringLiteral("room"), QStringLiteral("cutplay")};
    return e;
}

bool EditorModel::isProjectFile(const QString& path) {
    return projectExtensions().contains(QFileInfo(path).suffix().toLower());
}

bool EditorModel::confirmDiscard(bool quitting) {
    if (autosaves()) {
        // Quitting keeps untitled work in the recovery copy, which reopens next time.
        if (quitting || !isDirty_ || project_.clips.empty()) return true;
        if (!fileUrl_.isEmpty()) { autosaveNow(); return true; }
        if (host_->alert(QStringLiteral("Discard unsaved changes?"),
                         QStringLiteral("Your changes to “%1” will be lost.").arg(title()),
                         {QStringLiteral("Discard"), QStringLiteral("Cancel")}) != 0) return false;
        dropUnsaved();
        return true;
    }
    commitLive();
    if (!isDirty_ || (project_.clips.empty() && project_.assets.empty())) return true;
    switch (host_->alert(QStringLiteral("Save changes to “%1”?").arg(title()),
                         QStringLiteral("Your changes will be lost if you don't save them."),
                         {QStringLiteral("Save"), QStringLiteral("Cancel"), QStringLiteral("Don't Save")})) {
    case 0: save(); return !isDirty_;
    case 2: dropUnsaved(); return true;
    default: return false;
    }
}

void EditorModel::newProject() {
    // File › New: a new tab.
    if (newTabHandler()) { newTabHandler()(); return; }
    if (!confirmDiscard()) return;
    load(Project(), QString());
    QFile::remove(recoveryPath());
    setLastSession(std::nullopt);
}

QStringList EditorModel::recentProjects() const {
    return QSettings().value(QStringLiteral("recentProjects")).toStringList();
}

void EditorModel::noteRecent(const QString& path) {
    if (testLaunch) return;
    QStringList list = recentProjects();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > 10) list.removeLast();
    QSettings().setValue(QStringLiteral("recentProjects"), list);
    emit changed();
}

void EditorModel::openRecent(const QString& path) {
    if (!QFileInfo::exists(path)) {
        const QString msg = QStringLiteral("“%1” can't be found. It may have been moved or deleted.").arg(QFileInfo(path).fileName());
        host_->showError(msg);
        emit errorRaised(msg);
        return;
    }
    if (openHandler()) { openHandler()(path); return; }
    if (!confirmDiscard()) return;
    open(path);
}

void EditorModel::clearRecents() {
    QSettings().remove(QStringLiteral("recentProjects"));
    emit changed();
}

void EditorModel::openProject() {
    // With tabs, nothing open is closed: the project opens in a tab of its own.
    if (!openHandler() && !confirmDiscard()) return;
    const QString path = host_->chooseProjectFile();
    if (path.isEmpty()) return;
    if (openHandler()) openHandler()(path); else open(path);
}

void EditorModel::save() {
    if (fileUrl_.isEmpty()) return saveAs();
    writeTo(fileUrl_);
    noteRecent(fileUrl_);
}

void EditorModel::saveAs() {
    QString path = host_->chooseSaveFile(title() + QStringLiteral(".room"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".room");
    writeTo(path);
    noteRecent(path);
}

bool EditorModel::writeTo(const QString& path) {
    commitLive();
    Project saved = project_;
    saved.stampRelativePaths(QFileInfo(path).absolutePath());
    if (!writeAtomic(path, codec::saveBytes(SavedDocument{1, saved, {}}))) {
        const QString msg = QStringLiteral("Couldn't save to %1.").arg(QFileInfo(path).fileName());
        host_->showError(msg);
        emit errorRaised(msg);
        return false;
    }
    if (fileUrl_.isEmpty() || recoveryFor()) QFile::remove(recoveryPath());
    setRecoveryFor(std::nullopt);
    lastRecovered_.reset();
    fileUrl_ = QFileInfo(path).absoluteFilePath();
    isDirty_ = false;
    setLastSession(fileUrl_);
    emit changed();
    return true;
}

void EditorModel::load(const Project& p, const QString& path, bool offerLocate) {
    if (isPlaying_) pause();
    commitLive();
    undoStack_.clear();
    redoStack_.clear();
    redoKnown_.clear();
    selection_.clear();
    mediaSelection.clear();
    folderSelection.clear();
    mediaFolder.reset();
    playhead_ = 0;
    host_->expect(p.assets);
    // (The Mac makes bookmarks for older projects here; LX can't make macOS bookmarks.)
    project_ = p;
    fileUrl_ = path;
    isDirty_ = false;
    setView(std::nullopt, 0.0);
    fitRows();
    setNeedsRebuild();
    emit selectionChanged();
    emit playheadChanged(0);
    emit changed();
    if (offerLocate && !missingAssets().empty()) QTimer::singleShot(0, this, [this] { offerToLocateMissing(); });
}

std::vector<MediaAsset> EditorModel::missingAssets() const {
    std::vector<MediaAsset> out;
    for (const auto& a : project_.assets) {
        const QString p = a.playablePath && QFileInfo::exists(*a.playablePath) ? *a.playablePath : a.path;
        if (!QFileInfo::exists(p)) out.push_back(a);
    }
    return out;
}

void EditorModel::offerToLocateMissing() {
    const auto missing = missingAssets();
    if (missing.empty()) return;
    // A linked folder that's gone: located once, for everything in it.
    if (project_.folders) {
        for (const auto& f : *project_.folders) {
            if (MediaFolders::isFolder(f.path)) continue;
            int n = 0;
            for (const auto& m : missing) if (MediaFolders::contains(f.path, m.path)) ++n;
            if (n == 0) continue;
            const Uuid id = f.id;
            if (host_->alert(QStringLiteral("The folder “%1” is missing").arg(f.name()),
                             QStringLiteral("%1 item%2 be found. Locate the folder and everything in it is found again.")
                                 .arg(n).arg(n == 1 ? QStringLiteral(" in it can't") : QStringLiteral("s in it can't")),
                             {QStringLiteral("Locate…"), QStringLiteral("Not Now")}) == 0)
                host_->locateFolder(id);
            return;
        }
    }
    QStringList names;
    for (size_t i = 0; i < missing.size() && i < 6; ++i) names << QFileInfo(missing[i].path).fileName();
    const QString message = missing.size() == 1 ? QStringLiteral("1 media file is missing")
                                                : QStringLiteral("%1 media files are missing").arg(missing.size());
    const QString info = names.join(u'\n') + (missing.size() > size_t(names.size()) ? QStringLiteral("\n…") : QString())
        + QStringLiteral("\n\nLocate one and any others in the same folder are found too.");
    if (host_->alert(message, info, {QStringLiteral("Locate…"), QStringLiteral("Not Now")}) == 0) host_->locate(missing.front().id, true);
}

void EditorModel::offerLocateMissing() { offerToLocateMissing(); }

void EditorModel::clickMedia(const MediaPick& pick, const std::vector<MediaPick>& order, Qt::KeyboardModifiers modifiers) {
    std::vector<MediaPick> picked;
    for (const auto& a : project_.assets) if (mediaSelection.count(a.id)) picked.push_back(MediaPick::ofAsset(a.id));
    for (const auto& f : folderSelection) picked.push_back(MediaPick::ofFolder(f));
    auto indexOf = [&](const MediaPick& p) -> std::optional<size_t> {
        for (size_t i = 0; i < order.size(); ++i) if (order[i] == p) return i;
        return std::nullopt;
    };
    const auto a = mediaAnchor ? indexOf(*mediaAnchor) : std::nullopt;
    const auto b = indexOf(pick);
    if ((modifiers & Qt::ShiftModifier) && a && b) {
        picked.assign(order.begin() + long(std::min(*a, *b)), order.begin() + long(std::max(*a, *b)) + 1);
    } else if (modifiers & Qt::ControlModifier) {
        auto it = std::find(picked.begin(), picked.end(), pick);
        if (it != picked.end()) picked.erase(it); else picked.push_back(pick);
        mediaAnchor = pick;
    } else {
        picked = {pick};
        mediaAnchor = pick;
    }
    mediaSelection.clear();
    folderSelection.clear();
    for (const auto& p : picked) { if (p.asset) mediaSelection.insert(*p.asset); else folderSelection.insert(p.folder); }
    // One thing selected at a time: Delete then means these.
    setSelection({});
    emit mediaSelectionChanged();
}

void EditorModel::openMediaFolder(const std::optional<QString>& folder) {
    mediaSelection.clear();
    folderSelection.clear();
    mediaFolder = folder;
    emit mediaSelectionChanged();
}

} // namespace er
