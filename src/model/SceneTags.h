// SceneTags.swift: what a clip or photo shows ("pool", "beach", "dog", "sunset"), from a frame every couple of
// seconds, kept on disk by file for Media search and Magic. The searching (shows, matching, the everyday words for
// labels) is the Mac's, literally; what sees the frames differs (engine/SceneTagStore.h).
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <map>
#include <optional>
#include <vector>

namespace er {

struct SceneTags {
    struct Tag {
        QString name;
        /// How sure the looker is, at the frame it was surest (0 to 1).
        double confidence = 0;
        /// Times in the file where it shows.
        std::vector<double> times;
        bool operator==(const Tag&) const = default;
    };
    std::vector<Tag> tags;
    bool operator==(const SceneTags&) const = default;

    /// Keywords from a describing model, each where it was seen: tags of their own (a little less sure than the
    /// surest), or more times on a tag already there.
    void add(const std::vector<std::pair<double, QStringList>>& described);
    /// The surest few, for a short summary.
    QStringList top(int n) const;
    /// Whether one of its tags is this word, or starts with it while it's being typed ("po" for "pool").
    bool shows(const QString& word, bool typing = false) const;
    /// The tags that match a word (as `shows` does), for saying where in the file it shows.
    std::vector<Tag> matching(const QString& word) const;
    /// The same word in another form ("laughing" and "laughter"), for longer words.
    static bool sameStem(const QString& a, const QString& b);
    static QString singular(const QString& w);
    /// Everyday words for the labels, so a search for "door" finds a "portal".
    static const std::map<QString, QStringList>& aliases();

    QJsonObject json() const;
    static std::optional<SceneTags> fromJson(const QJsonObject& o);
};

} // namespace er
