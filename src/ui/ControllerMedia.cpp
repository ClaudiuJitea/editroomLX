// The Media panel's file work (EditorModel.swift: importFiles, linkFolder, syncFolders, reloadMedia, locate,
// relink, relinkFolder). LX can't resolve macOS bookmarks, so folders and files renamed or moved on their
// drive aren't followed on their own: Locate… finds them (see docs/PARITY.md).
#include "Controller.h"
#include "model/MediaFolders.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QProcess>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace er {

/// Starts following a folder brought in whole. Returns the new linked folder, or none when it's already inside
/// one (`nested`). Linked folders inside it become part of it.
std::pair<std::optional<MediaFolder>, bool> Controller::linkFolder(const QString& path) {
    EditorModel& m = *model_;
    std::optional<MediaFolder> made;
    bool nested = false;
    m.changeLibrary([&](Project& p) {
        // Everything left out of it comes back: it was brought in again on purpose.
        if (p.hiddenMedia) {
            std::erase_if(*p.hiddenMedia, [&](const QString& h) { return MediaFolders::contains(path, h) || MediaFolders::contains(h, path); });
            if (p.hiddenMedia->empty()) p.hiddenMedia.reset();
        }
        if (p.linkedFolder(path)) { nested = true; return; }
        MediaFolder f;
        f.path = path;
        std::vector<MediaFolder> list;
        for (const auto& o : p.folders.value_or(std::vector<MediaFolder>{})) if (!MediaFolders::contains(path, o.path)) list.push_back(o);
        list.push_back(f);
        p.folders = list;
        made = f;
    });
    return {made, nested};
}

void Controller::importFiles(const QStringList& paths, bool offer, std::function<void(std::vector<MediaAsset>)> then) {
    if (paths.isEmpty()) return;
    ++importCount_;
    emit importChanged();
    std::vector<MediaFolder> linked;
    std::optional<QString> opened;
    QStringList folders, files;
    for (const QString& raw : paths) {
        const QString path = QFileInfo(raw).absoluteFilePath();
        if (MediaFolders::isFolder(path)) {
            auto [folder, nested] = linkFolder(path);
            if (folder) { linked.push_back(*folder); importingFolders_.insert(folder->id); }
            if (nested) opened = path;
            folders << path;
        } else {
            // Brought in by hand: no longer left out of its folder.
            model_->changeLibrary([&](Project& p) {
                if (!p.hiddenMedia) return;
                std::erase_if(*p.hiddenMedia, [&](const QString& h) { return MediaFolders::contains(h, path); });
                if (p.hiddenMedia->empty()) p.hiddenMedia.reset();
            });
            files << path;
        }
    }
    const int requested = int(paths.size());
    (void)QtConcurrent::run([this, folders, files, linked, opened, offer, then, requested] {
        QStringList all = files;
        for (const QString& f : folders) all += MediaFolders::mediaFiles(f);
        QMetaObject::invokeMethod(this, [this, all, linked, opened, offer, then, requested] {
            // Files left out of a linked folder stay out when it's looked at again.
            QStringList todo;
            for (const QString& p : all) if (!model_->project().isHidden(p)) todo << p;
            importCount_ += int(todo.size()) - 1;
            emit importChanged();
            struct Run {
                std::vector<std::optional<MediaAsset>> made;
                QStringList failed;
                int left = 0;
            };
            auto run = std::make_shared<Run>();
            run->made.resize(size_t(todo.size()));
            run->left = int(todo.size());
            auto finish = [this, run, linked, opened, offer, then, requested] {
                for (const auto& f : linked) importingFolders_.erase(f.id);
                model_->changeLibrary([this](Project& p) { p.pruneFolders(importingFolders_); });
                for (const auto& f : linked) {
                    bool kept = false;
                    for (const auto& o : model_->project().folders.value_or(std::vector<MediaFolder>{})) if (o.id == f.id) kept = true;
                    if (!kept) importError_ = QStringLiteral("There's no media in “%1”.").arg(f.name());
                }
                if (!run->failed.isEmpty()) {
                    QStringList shown = run->failed.mid(0, 8);
                    importError_ = QStringLiteral("Couldn't import ") + shown.join(QStringLiteral(", "))
                        + (run->failed.size() > 8 ? QStringLiteral(" and %1 more").arg(run->failed.size() - 8) : QString()) + u'.';
                }
                std::vector<MediaAsset> imported;
                for (const auto& a : run->made) if (a) imported.push_back(*a);
                // One thing brought in by hand: shown where it sits in Media.
                if (offer && requested == 1) {
                    const MediaTree tree(model_->project());
                    if (opened && tree.exists(*opened)) {
                        model_->openMediaFolder(*opened);
                    } else if (linked.empty() && !imported.empty()) {
                        model_->openMediaFolder(tree.folderOf(imported.front()));
                        model_->mediaSelection = {imported.front().id};
                        model_->mediaAnchor = MediaPick::ofAsset(imported.front().id);
                        emit model_->mediaSelectionChanged();
                    }
                }
                emit importChanged();
                if (then) then(imported);
            };
            if (todo.isEmpty()) { finish(); return; }
            for (int i = 0; i < todo.size(); ++i) {
                const QString path = todo[i];
                bool known = false;
                for (const auto& a : model_->project().assets) {
                    if (a.path != path) continue;
                    run->made[size_t(i)] = a;
                    known = true;
                }
                if (known || pendingImports_.count(path)) {
                    --importCount_;
                    if (--run->left == 0) finish();
                    continue;
                }
                pendingImports_.insert(path);
                (void)QtConcurrent::run([this, path, i, run, offer, finish] {
                    auto asset = media_.importFile(path);
                    QMetaObject::invokeMethod(this, [this, asset, path, i, run, offer, finish] {
                        pendingImports_.erase(path);
                        --importCount_;
                        if (asset) {
                            model_->changeLibrary([&](Project& p) { p.assets.push_back(*asset); });
                            run->made[size_t(i)] = *asset;
                            if (offer) offerMatch(*asset);
                        } else {
                            // Not tried again when its folder is next looked at.
                            unreadable_.insert(path);
                            run->failed << QFileInfo(path).fileName();
                        }
                        emit importChanged();
                        if (--run->left == 0) finish();
                    }, Qt::QueuedConnection);
                });
            }
        }, Qt::QueuedConnection);
    });
}

void Controller::syncFolders(std::function<void()> then) {
    if (syncingFolders_) { if (then) then(); return; }
    std::vector<MediaFolder> folders = model_->project().folders.value_or(std::vector<MediaFolder>{});
    if (folders.empty()) { if (then) then(); return; }
    syncingFolders_ = true;
    (void)QtConcurrent::run([this, folders, then] {
        std::vector<QString> found;
        for (const auto& f : folders) if (MediaFolders::isFolder(f.path)) for (const QString& p : MediaFolders::mediaFiles(f.path)) found.push_back(p);
        QMetaObject::invokeMethod(this, [this, found, then] {
            syncingFolders_ = false;
            std::set<QString> have;
            for (const auto& a : model_->project().assets) have.insert(a.path);
            QStringList added;
            for (const QString& p : found)
                if (!have.count(p) && !model_->project().isHidden(p) && !pendingImports_.count(p) && !unreadable_.count(p)) added << p;
            if (auto open = model_->mediaFolder; open && !MediaTree(model_->project()).exists(*open)) model_->openMediaFolder(std::nullopt);
            if (!added.isEmpty()) importFiles(added, false, [then](std::vector<MediaAsset>) { if (then) then(); });
            else if (then) then();
        }, Qt::QueuedConnection);
    });
}

void Controller::reloadMedia(const IdSet& ids) {
    std::vector<MediaAsset> olds;
    for (const auto& a : model_->project().assets) if (ids.count(a.id) && QFileInfo::exists(a.path)) olds.push_back(a);
    if (olds.empty()) return;
    (void)QtConcurrent::run([this, olds] {
        std::vector<MediaAsset> fresh;
        for (const auto& old : olds) if (auto f = media_.importFile(old.path, &old)) fresh.push_back(*f);
        QMetaObject::invokeMethod(this, [this, fresh] {
            IdSet reloaded;
            bool described = false;
            for (const auto& f : fresh) {
                reloaded.insert(f.id);
                thumbs_->forget(f.id);
                scrubCache_.forget(f.id);
                transcripts_.forget(media_.soundPath(f.id));
                scenes_.forget(media_.readPath(f));
                model_->changeLibrary([&](Project& p) {
                    for (auto& a : p.assets) {
                        if (a.id != f.id) continue;
                        MediaAsset was = a;
                        a.playablePath = f.playablePath;
                        a.duration = f.duration;
                        a.width = f.width;
                        a.height = f.height;
                        a.fps = f.fps;
                        a.hasAudio = f.hasAudio;
                        if (!(was == a)) described = true;
                    }
                });
            }
            if (reloaded.empty()) return;
            sourcePreview_->clear();
            if (described) model_->mutate([&](Project& p) { p.fitClips(reloaded); });
            model_->setNeedsRebuild();
        }, Qt::QueuedConnection);
    });
}

void Controller::reloadFolder(const QString& folder) {
    syncFolders([this, folder] {
        IdSet ids;
        for (const auto& a : MediaTree(model_->project()).assetsUnder(folder, model_->project().assets)) ids.insert(a.id);
        reloadMedia(ids);
    });
}

bool Controller::relink(const Uuid& id, const QString& path) {
    const MediaAsset* oldp = model_->project().asset(id);
    if (!oldp) return false;
    const MediaAsset old = *oldp;
    auto fresh = media_.importFile(path, &old);
    if (!fresh) return false;
    model_->changeLibrary([&](Project& p) { for (auto& a : p.assets) if (a.id == id) a = *fresh; });
    thumbs_->forget(id);
    scrubCache_.forget(id);
    // In a linked folder that's gone: the folder is where this file is now, if it's the same place in it.
    if (const MediaFolder* root = model_->project().linkedFolder(old.path); root && !MediaFolders::isFolder(root->path)) {
        const QString inside = old.path.mid(root->path.size());
        const QString now = QFileInfo(path).absoluteFilePath();
        if (now.endsWith(inside)) {
            const QString moved = now.left(now.size() - inside.size());
            if (MediaFolders::isFolder(moved)) relinkFolder(root->path, moved);
        }
    }
    const QDir dir = QFileInfo(path).dir();
    for (const auto& other : model_->missingAssets()) {
        if (other.id == id) continue;
        const QString candidate = dir.filePath(QFileInfo(other.path).fileName());
        if (!QFileInfo::exists(candidate)) continue;
        if (auto found = media_.importFile(candidate, &other)) {
            model_->changeLibrary([&](Project& p) { for (auto& a : p.assets) if (a.id == other.id) a = *found; });
            thumbs_->forget(other.id);
            scrubCache_.forget(other.id);
        }
    }
    // Undo snapshots still hold the old paths, so they can't be kept.
    model_->changeLibrary([](Project&) {}, true);
    model_->setNeedsRebuild();
    return true;
}

void Controller::relinkFolder(const QString& from, const QString& to) {
    IdSet ids;
    for (const auto& a : model_->project().assets) if (MediaFolders::contains(from, a.path)) ids.insert(a.id);
    model_->changeLibrary([&](Project& p) { p.movePaths(from, to); }, true);
    if (auto open = model_->mediaFolder; open && MediaFolders::contains(from, *open)) model_->openMediaFolder(to + open->mid(from.size()));
    for (const auto& id : ids) { thumbs_->forget(id); scrubCache_.forget(id); }
    // Open everything in it again at its new place, then bring in anything new.
    for (const auto& id : ids) {
        const MediaAsset* a = model_->project().asset(id);
        if (!a || !QFileInfo::exists(a->path)) continue;
        const MediaAsset old = *a;
        if (auto fresh = media_.importFile(old.path, &old)) {
            MediaAsset f = *fresh;
            f.relativePath = old.relativePath;
            model_->changeLibrary([&](Project& p) { for (auto& x : p.assets) if (x.id == id) x = f; });
        }
    }
    sourcePreview_->clear();
    model_->setNeedsRebuild();
    syncFolders();
}

void Controller::locate(const Uuid& id, bool thenOthers) {
    const MediaAsset* a = model_->project().asset(id);
    if (!a) return;
    const QString original = QFileInfo(a->path).fileName();
    QFileDialog d(window_, QStringLiteral("Locate “%1”").arg(original));
    d.setFileMode(QFileDialog::ExistingFile);
    d.setLabelText(QFileDialog::Accept, QStringLiteral("Locate"));
    d.selectFile(original);
    if (d.exec() != QDialog::Accepted || d.selectedFiles().isEmpty()) return;
    const QString path = d.selectedFiles().front();
    if (relink(id, path)) {
        if (thenOthers) { auto missing = model_->missingAssets(); if (!missing.empty()) locate(missing.front().id, true); }
    } else {
        setImportError(QStringLiteral("“%1” can't be used in place of “%2”.").arg(QFileInfo(path).fileName(), original));
    }
}

void Controller::locateFolder(const Uuid& id) {
    std::optional<MediaFolder> f;
    for (const auto& x : model_->project().folders.value_or(std::vector<MediaFolder>{})) if (x.id == id) f = x;
    if (!f) return;
    const QString dir = QFileDialog::getExistingDirectory(window_, QStringLiteral("Locate the folder “%1”").arg(f->name()));
    if (dir.isEmpty()) return;
    relinkFolder(f->path, QDir(dir).absolutePath());
    if (!model_->missingAssets().empty()) model_->offerLocateMissing();
}

void Controller::showInFileManager(const QString& path) {
    // The freedesktop file manager interface selects the file; without it, its folder opens.
    QProcess dbus;
    dbus.start(QStringLiteral("dbus-send"), {QStringLiteral("--session"), QStringLiteral("--print-reply"), QStringLiteral("--dest=org.freedesktop.FileManager1"),
                                            QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                                            QStringLiteral("array:string:") + QUrl::fromLocalFile(path).toString(), QStringLiteral("string:")});
    if (dbus.waitForFinished(2000) && dbus.exitCode() == 0) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
}

} // namespace er
