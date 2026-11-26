// MediaFolders.swift: linked folders and the Media panel's tree.
#pragma once
#include "Project.h"
#include <QHash>

namespace er {

namespace MediaFolders {
bool isFolder(const QString& path);
/// Whether `path` is `folder` or inside it.
bool contains(const QString& folder, const QString& path);
/// The media files in a folder and the folders inside it (not hidden files, not inside packages), in
/// name order. Linux has no packages (bundles); directories ending in the Mac's package extensions
/// (.app, .photoslibrary, .fcpbundle…) are skipped so a Mac drive mounted here behaves the same.
QStringList mediaFiles(const QString& path);
/// A file Editroom can bring in, by its type.
bool isMedia(const QString& path);
/// Finder's `localizedStandardCompare` order (case-insensitive, numbers by value).
bool standardLess(const QString& a, const QString& b);
} // namespace MediaFolders

/// The Media panel's folders, worked out from the project: the linked folders at the top, and inside
/// each, the folders its files are in. A folder is named by its path.
class MediaTree {
public:
    explicit MediaTree(const Project& project);
    std::vector<MediaFolder> roots;

    std::optional<QString> folderOf(const MediaAsset& asset) const;
    const MediaFolder* root(const QString& folder) const;
    bool exists(const QString& folder) const;
    QStringList folders(const std::optional<QString>& folder) const;
    std::vector<MediaAsset> assetsIn(const std::optional<QString>& folder, const std::vector<MediaAsset>& all) const;
    std::vector<MediaAsset> assetsUnder(const QString& folder, const std::vector<MediaAsset>& all) const;
    int count(const QString& folder) const;
    /// "B-roll / Drone".
    QString label(const QString& folder) const;
    QStringList trail(const QString& folder) const;
    static QString name(const QString& folder);

private:
    QHash<QUuid, QString> place_;
};

} // namespace er
