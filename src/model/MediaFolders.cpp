#include "MediaFolders.h"
#include <QCollator>
#include <QDirIterator>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSet>

namespace er {

bool MediaFolders::isFolder(const QString& path) { return QFileInfo(path).isDir(); }

bool MediaFolders::contains(const QString& folder, const QString& path) {
    return path == folder || path.startsWith(folder.endsWith(u'/') ? folder : folder + u'/');
}

bool MediaFolders::standardLess(const QString& a, const QString& b) {
    static thread_local QCollator c = [] {
        QCollator x(QLocale::system());
        x.setNumericMode(true);
        x.setCaseSensitivity(Qt::CaseInsensitive);
        return x;
    }();
    return c.compare(a, b) < 0;
}

bool MediaFolders::isMedia(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext.isEmpty()) return false;
    // Formats ffmpeg reads that the type system may not know (the Mac's extra list).
    static const QSet<QString> extra = {"mkv", "webm", "avi", "flv", "wmv", "mts", "m2ts", "ts", "mxf", "ogv",
                                        "ogg", "opus", "flac", "3gp", "mpg", "mpeg", "vob", "f4v"};
    if (extra.contains(ext)) return true;
    static QMimeDatabase db;
    // By extension only, like UTType(filenameExtension:).
    for (const QMimeType& t : db.mimeTypesForFileName(QStringLiteral("x.") + ext)) {
        const QString n = t.name();
        if (n.startsWith(u"video/") || n.startsWith(u"audio/") || n.startsWith(u"image/")) return true;
        for (const QString& p : t.allAncestors())
            if (p.startsWith(u"video/") || p.startsWith(u"audio/") || p.startsWith(u"image/")) return true;
    }
    return false;
}

static bool isPackage(const QFileInfo& d) {
    static const QSet<QString> exts = {"app", "bundle", "framework", "photoslibrary", "fcpbundle", "imovielibrary",
                                       "tvlibrary", "musiclibrary", "plugin", "kext", "pkg", "rtfd", "lproj"};
    return exts.contains(d.suffix().toLower());
}

static void walk(const QString& dir, const QString& spelled, QStringList& out) {
    QDir d(dir);
    const auto entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo& e : entries) {
        if (e.fileName().startsWith(u'.')) continue; // skipsHiddenFiles
        const QString p = spelled + u'/' + e.fileName();
        if (e.isDir()) {
            if (isPackage(e)) continue; // skipsPackageDescendants
            walk(e.filePath(), p, out);
        } else if (e.isFile() && MediaFolders::isMedia(e.fileName())) {
            out << p;
        }
    }
}

QStringList MediaFolders::mediaFiles(const QString& path) {
    QStringList files;
    if (!isFolder(path)) return files;
    // Files are kept in the folder's own spelling, so files and folder always line up.
    walk(path, path, files);
    std::sort(files.begin(), files.end(), standardLess);
    return files;
}

// MARK: MediaTree

MediaTree::MediaTree(const Project& project) {
    roots = project.folders.value_or(std::vector<MediaFolder>{});
    std::stable_sort(roots.begin(), roots.end(), [](const MediaFolder& a, const MediaFolder& b) {
        return MediaFolders::standardLess(a.name(), b.name());
    });
    for (const auto& a : project.assets)
        if (project.linkedFolder(a.path)) place_.insert(a.id, QFileInfo(a.path).path());
}

std::optional<QString> MediaTree::folderOf(const MediaAsset& asset) const {
    auto it = place_.find(asset.id);
    if (it == place_.end()) return std::nullopt;
    return *it;
}

const MediaFolder* MediaTree::root(const QString& folder) const {
    const MediaFolder* best = nullptr;
    for (const auto& r : roots)
        if (MediaFolders::contains(r.path, folder) && (!best || best->path.size() < r.path.size())) best = &r;
    return best;
}

bool MediaTree::exists(const QString& folder) const {
    for (const auto& r : roots) if (r.path == folder) return true;
    for (const auto& p : place_) if (MediaFolders::contains(folder, p)) return true;
    return false;
}

QStringList MediaTree::folders(const std::optional<QString>& folder) const {
    QStringList out;
    if (!folder) { for (const auto& r : roots) out << r.path; return out; }
    QSet<QString> names;
    const QString prefix = *folder + u'/';
    for (const auto& p : place_) {
        if (!p.startsWith(prefix)) continue;
        const QStringList parts = p.mid(prefix.size()).split(u'/', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) names.insert(parts.first());
    }
    QStringList sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end(), MediaFolders::standardLess);
    for (const auto& n : sorted) out << prefix + n;
    return out;
}

std::vector<MediaAsset> MediaTree::assetsIn(const std::optional<QString>& folder, const std::vector<MediaAsset>& all) const {
    std::vector<MediaAsset> out;
    for (const auto& a : all) if (folderOf(a) == folder) out.push_back(a);
    return out;
}

std::vector<MediaAsset> MediaTree::assetsUnder(const QString& folder, const std::vector<MediaAsset>& all) const {
    std::vector<MediaAsset> out;
    for (const auto& a : all) if (auto f = folderOf(a); f && MediaFolders::contains(folder, *f)) out.push_back(a);
    return out;
}

int MediaTree::count(const QString& folder) const {
    int n = 0;
    for (const auto& p : place_) if (MediaFolders::contains(folder, p)) ++n;
    return n;
}

QString MediaTree::label(const QString& folder) const {
    const MediaFolder* r = root(folder);
    if (!r) return name(folder);
    QStringList parts{r->name()};
    parts += folder.mid(r->path.size()).split(u'/', Qt::SkipEmptyParts);
    return parts.join(QStringLiteral(" / "));
}

QStringList MediaTree::trail(const QString& folder) const {
    const MediaFolder* r = root(folder);
    if (!r) return {folder};
    QStringList out{r->path};
    QString p = r->path;
    for (const auto& part : folder.mid(r->path.size()).split(u'/', Qt::SkipEmptyParts)) {
        p += u'/' + part;
        out << p;
    }
    return out;
}

QString MediaTree::name(const QString& folder) { return QFileInfo(folder).fileName(); }

} // namespace er
