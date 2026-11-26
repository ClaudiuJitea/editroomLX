#include "LocalBrain.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace er::LocalBrain {

namespace {
bool any(const QString& t, std::initializer_list<const char*> words) {
    for (const char* w : words) if (t.contains(QLatin1String(w))) return true;
    return false;
}
bool any(const QString& t, const QStringList& words) {
    for (const QString& w : words) if (t.contains(w)) return true;
    return false;
}
bool matches(const QString& t, const QString& pattern) { return QRegularExpression(pattern).match(t).hasMatch(); }
QStringList splitOn(const QString& t, const std::function<bool(QChar)>& keep) {
    QStringList out;
    QString cur;
    for (QChar c : t) {
        if (keep(c)) cur += c;
        else if (!cur.isEmpty()) { out << cur; cur.clear(); }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}
QString bareName(const QString& name) { const int dot = name.lastIndexOf(u'.'); return dot > 0 ? name.left(dot) : name; }
} // namespace

const QStringList looks = {"Warm film", "Golden hour", "Film print", "Teal and orange", "Pastel", "Bleach bypass", "Mono", "Cool night", "Faded"};

bool isGeneral(const QString& request) {
    return any(request.toLower(), {"improve", "better", "tighten", "tidy", "clean", "polish", "fix", "best", "nicer", "good", "shorter", "trim",
                                    "cut it down", "make it work", "edit it", "magic", "beat", "rhythm", "recut", "re-cut", "re-edit"});
}

bool asksJudgement(const QString& request) {
    const QString t = request.toLower();
    return t.contains(u'?') || any(t, {"do you think", "should i", "should we", "would it", "if so", "if you think", "worth", "any good",
                                       "makes sense", "if it fits", "where it fits", "if needed", "only if"});
}

std::optional<int> shotCount(const QString& request) {
    QString t = request.toLower();
    static const std::vector<std::pair<const char*, int>> words = {{"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
        {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"fifteen", 15}, {"twenty", 20}, {"thirty", 30}};
    for (const auto& [w, n] : words) t.replace(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QLatin1String(w))), QString::number(n));
    static const QRegularExpression re(QStringLiteral("(\\d+)\\s*(\\+\\s*)?(different |unique |separate |short |quick |fast )?(clips|shots|cuts|scenes|videos)"));
    const auto m = re.match(t);
    if (!m.hasMatch()) return std::nullopt;
    const int n = m.captured(1).toInt();
    if (n < 2 || n > 200) return std::nullopt;
    return n;
}

bool wantsEvery(const QString& request) {
    const QString t = request.toLower();
    const QString things = QStringLiteral("(clips|videos|shots|footage|media|files)");
    return matches(t, QStringLiteral("\\b(all|every one of|each of)\\s+(of\\s+)?(the\\s+|my\\s+|our\\s+)?") + things)
        || matches(t, QStringLiteral("\\b(every|each)\\s+(single\\s+)?(clip|video|shot|file)\\b"));
}

bool wantsLibrary(const QString& request) {
    return any(request.toLower(), {"most of the clips", "most clips", "all the clips", "all clips", "all of the clips", "all my clips", "every clip",
        "more clips", "more shots", "other clips", "other shots", "the rest of the clips", "whole library", "all my media", "media folder",
        "all the media", "all of my", "from scratch", "start over", "make a new", "create a new", "new video", "new cut", "new edit",
        "create ", "montage"});
}

std::optional<QString> quotedWords(const QString& request) {
    static const QStringList patterns = {QStringLiteral("\"([^\"]{1,120})\""), QStringLiteral("“([^”]{1,120})”"),
                                         QStringLiteral("(?:^|\\s)'(.{1,120}?)'(?=$|\\s|[.,!?;:])"), QStringLiteral("(?:^|\\s)‘(.{1,120}?)’(?=$|\\s|[.,!?;:])")};
    for (const QString& p : patterns) {
        const auto m = QRegularExpression(p).match(request);
        if (!m.hasMatch()) continue;
        const QString words = m.captured(1).trimmed();
        if (!words.isEmpty()) return words;
    }
    return std::nullopt;
}

std::optional<QString> titleAsked(const QString& request) {
    const QString t = request.toLower();
    const bool quoted = quotedWords(request).has_value();
    const bool asks = (quoted && t.contains(QLatin1String("caption")))
        || any(t, {"title", "heading", "text on", "words on", "add text", "put text", "some text", "call it", "name it", "text saying", "text that says"});
    const bool refuses = any(t, {"no title", "without a title", "without title", "no text", "without text", "remove the title", "take out the title"});
    if (!asks || refuses) return std::nullopt;
    return quotedWords(request).value_or(QString());
}

bool wantsSoundEffects(const QString& request) {
    return any(request.toLower(), {"sfx", "sound effect", "sound fx", "soundfx", "fx sound", "whoosh", "swoosh", "foley", "sound design"});
}

bool wantsLevels(const QString& request) {
    const QString t = request.toLower();
    const bool about = any(t, {"audio", "sound", "volume", "loud", "level", "clip", "voice", "music", "db"});
    const bool even = any(t, {"equaliz", "equalis", "normaliz", "normalis", "even out", "same level", "same volume", "same loudness", "same output",
                              "consistent", "match", "balance", "level out", "level the", "levels the same", "too quiet and"});
    return about && even;
}

std::vector<MediaAsset> soundEffects(const Project& p) {
    std::vector<MediaAsset> out;
    for (const auto& a : p.assets) if (a.kind == MediaKind::audio && a.duration > 0.05 && a.duration < 10) out.push_back(a);
    return out;
}

std::optional<double> speed(const QString& request) {
    const QString t = request.toLower();
    const bool about = any(t, {"speed", "faster", "slower", "fast", "slow", "sped", "timelapse", "time lapse", "playback"});
    if (any(t, {"slow motion", "slow-mo", "slowmo"})) return 0.5;
    if (any(t, {"double speed", "twice as fast"})) return 2;
    if (any(t, {"half speed", "half as fast"})) return 0.5;
    if (any(t, {"normal speed", "real time", "realtime"})) return 1;
    if (!about) return std::nullopt;
    if (auto m = QRegularExpression(QStringLiteral("(\\d+(\\.\\d+)?)\\s*%")).match(t); m.hasMatch())
        return std::clamp(m.captured(1).toDouble() / 100, 0.05, 100.0);
    if (auto m = QRegularExpression(QStringLiteral("(\\d+(\\.\\d+)?)\\s*(x|times)\\b")).match(t); m.hasMatch())
        return std::clamp(m.captured(1).toDouble(), 0.05, 100.0);
    return std::nullopt;
}

void applyMedia(LocalRecipe& r, const std::vector<MediaAsset>& named, const QString& action, const QString& request, const std::set<Uuid>& pointed,
                bool hasEdit) {
    if (named.empty()) return;
    const QString text = request.toLower();
    const bool only = any(text, {"only", "just ", "nothing else", "nothing but", "except", "solely", "keep ", "use the ", "using the "});
    const bool remove = any(text, {"remove", "take out", "without", "delete", "drop", "get rid", "cut out", "no more", "lose "});
    const bool explicitly = std::all_of(named.begin(), named.end(), [&](const MediaAsset& a) { return pointed.count(a.id) > 0; });
    const bool everyday = std::all_of(named.begin(), named.end(), [](const MediaAsset& a) {
        const QString b = bareName(a.name);
        return std::all_of(b.begin(), b.end(), [](QChar c) { return c.isLetter() || c == u' '; });
    });
    if (everyday && !explicitly && !only && !remove) return;
    const bool amongOthers = any(text, {"also", " too", "include", "as well", "most", "all ", "every", "the rest", "other"});
    const QString picked = !explicitly && action == QLatin1String("remove") && !remove ? QStringLiteral("only") : action;
    const QString fallback = hasEdit ? QStringLiteral("unchanged") : !explicitly ? picked
                           : !amongOthers && action != QLatin1String("remove") ? QStringLiteral("only") : QStringLiteral("unchanged");
    const QString mode = only && !remove ? QStringLiteral("only") : remove && !only ? QStringLiteral("remove") : fallback;
    std::set<Uuid> ids;
    for (const auto& a : named) ids.insert(a.id);
    if (mode == QLatin1String("only")) r.keepOnly = ids;
    else if (mode == QLatin1String("remove")) r.takeOut = ids;
    else return;
    r.mediaNames.clear();
    for (const auto& a : named) r.mediaNames << a.name;
    r.clearRest = mode == QLatin1String("only") && any(text, {"nothing else", "everything else", "anything else", "on the timeline", "on timeline"});
    for (const auto& a : named) if (r.cannotDo.toLower().contains(a.name.toLower())) { r.cannotDo.clear(); break; }
}

namespace {
const QStringList lengthWords = {"second", "sec", "minute", "min", "long", "short", "length", "s "};
const QStringList paceWords = {"pace", "pacing", "calm", "slow", "relax", "chill", "gentle", "fast", "quick", "punch", "snappy", "energ", "rhythm", "tempo"};
const QStringList lookWords = {"look", "grade", "colour", "color", "film", "warm", "cool", "mono", "black and white", "b&w", "bw", "vintage", "fade",
                               "pastel", "teal", "orange", "golden", "night", "bleach", "filter", "tone", "cinematic", "moody", "natural", "lut"};
const QStringList musicWords = {"music", "song", "soundtrack", "track", "tune", "audio", "beat"};
const QStringList frameWords = {"vertical", "portrait", "tiktok", "tik tok", "reel", "shorts", "story", "stories", "phone", "horizontal", "landscape",
                                "widescreen", "youtube", "square", "instagram", "9:16", "16:9", "1:1"};
} // namespace

LocalRecipe checked(const LocalRecipe& r, const QString& request) {
    const QString text = request.toLower();
    auto says = [&](const QStringList& w) { return any(text, w); };
    LocalRecipe out = r;
    const bool timed = matches(text, QStringLiteral("\\d+(\\.\\d+)?\\s*(s|sec|secs|second|seconds|m|min|mins|minute|minutes)\\b|\\d+:\\d\\d"));
    const bool worded = says({"long", "length", "duration"}) && std::any_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); });
    if (out.seconds && !(timed || worded)) out.seconds.reset();
    if (out.pace != LocalRecipe::Pace::unchanged && !says(paceWords)) out.pace = LocalRecipe::Pace::unchanged;
    if (out.look && !says(lookWords)) out.look.reset();
    if (out.music != LocalRecipe::Music::unchanged && !says(musicWords)) out.music = LocalRecipe::Music::unchanged;
    if (out.music == LocalRecipe::Music::off && !says({"no ", "without", "remove", "off", "mute", "silent", "drop", "take out"})) out.music = LocalRecipe::Music::unchanged;
    if (out.frame != LocalRecipe::Frame::unchanged && !says(frameWords)) out.frame = LocalRecipe::Frame::unchanged;
    QStringList handled;
    if (out.music != LocalRecipe::Music::unchanged) handled += musicWords;
    if (out.look) handled += lookWords;
    if (out.frame != LocalRecipe::Frame::unchanged) handled += frameWords;
    if (out.pace != LocalRecipe::Pace::unchanged) handled += paceWords;
    if (out.seconds) handled += lengthWords;
    QStringList requestWords;
    for (const QString& w : splitOn(text, [](QChar c) { return c.isLetter(); })) if (w.size() >= 4) requestWords << w;
    QStringList kept;
    for (QString part : r.cannotDo.split(u',')) {
        for (QString piece : part.split(QStringLiteral(" and "))) {
            piece = piece.trimmed();
            if (piece.isEmpty()) continue;
            const QString p = piece.toLower();
            bool shares = false;
            for (const QString& w : splitOn(p, [](QChar c) { return c.isLetter(); }))
                if (w.size() >= 4) for (const QString& rw : requestWords) if (rw.startsWith(w.left(5))) shares = true;
            if (shares && !any(p, handled)) kept << piece;
        }
    }
    out.cannotDo = kept.join(QStringLiteral(", "));
    return out;
}

std::vector<MediaAsset> named(const QString& request, const std::vector<MediaAsset>& media) {
    const QString text = request.toLower();
    const QStringList words = splitOn(text, [](QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u'-'; });
    std::vector<MediaAsset> out;
    for (const auto& a : media) {
        const QString bare = bareName(a.name.toLower());
        if (bare.size() >= 3
            && QRegularExpression(QStringLiteral("(?<![\\p{L}\\p{N}])") + QRegularExpression::escape(bare) + QStringLiteral("(?![\\p{L}\\p{N}])")).match(text).hasMatch()) {
            out.push_back(a);
            continue;
        }
        bool hit = false;
        for (const QString& n : splitOn(bare, [](QChar c) { return c.isDigit(); })) if (n.size() >= 3 && words.contains(n)) hit = true;
        if (hit) out.push_back(a);
    }
    return out;
}

std::vector<MediaAsset> showing(const QStringList& words, const std::vector<MediaAsset>& media, const Shows& shows) {
    static const QStringList general = {"people", "person", "adult"};
    std::vector<std::pair<MediaAsset, double>> scored;
    for (const auto& a : media) {
        auto t = shows(a.id);
        double score = 0;
        if (t) for (const QString& w : words) {
            if (!t->shows(w)) continue;
            const auto tags = t->matching(w);
            // A particular label counts for more the surer the looker is of it.
            std::optional<double> most;
            for (const auto& g : tags) if (!general.contains(g.name)) most = std::max(most.value_or(-1e9), g.confidence);
            const double particular = most.value_or(tags.empty() ? 1 : 0);
            score += 1 + 0.5 * particular;
        }
        scored.push_back({a, score});
    }
    double best = 0;
    for (const auto& s : scored) best = std::max(best, s.second);
    std::vector<MediaAsset> out;
    if (best <= 0) return out;
    for (const auto& s : scored) if (s.second == best) out.push_back(s.first);
    return out;
}

QString askedPhrase(const QString& request, const QStringList& matched) {
    const QStringList words = splitOn(request.toLower(), [](QChar c) { return c.isLetter() || c == u'-' || c == u'\''; });
    int first = -1, last = -1;
    for (int i = 0; i < words.size(); ++i) if (matched.contains(words[i])) { if (first < 0) first = i; last = i; }
    if (first < 0) return {};
    static const QStringList describing = {"little", "small", "big", "young", "old", "baby", "red", "blue", "white", "black", "happy", "funny", "cute"};
    if (first > 0 && describing.contains(words[first - 1])) --first;
    return words.mid(first, last - first + 1).join(u' ');
}

std::map<Uuid, std::vector<double>> moments(const QStringList& words, const std::vector<MediaAsset>& media, const Shows& shows) {
    std::map<Uuid, std::vector<double>> out;
    for (const auto& a : media) {
        if (a.kind != MediaKind::video) continue;
        auto tags = shows(a.id);
        if (!tags) continue;
        std::set<double> times;
        for (const QString& w : words) for (const auto& g : tags->matching(w)) times.insert(g.times.begin(), g.times.end());
        if (!times.empty()) out[a.id] = std::vector<double>(times.begin(), times.end());
    }
    return out;
}

bool titleAtEnd(const QString& request) {
    return any(request.toLower(), {"at the end", "the end", "at end", "in the end", "to the end", "end of", "ending", "end screen", "end card",
                                   "end title", "outro", "closing", "at the close", "last shot", "at the finish", "final shot", "finale"});
}

bool captionsRemoved(const QString& request) {
    return any(request.toLower(), {"remove", "delete", "take off", "take away", "get rid", "no caption", "no subtitle", "without caption",
                                   "without subtitle", "turn off", "hide the caption", "hide caption"});
}

bool captionsWithCut(const QString& request) {
    return any(request.toLower(), {"caption", "subtitle"}) && !captionsRemoved(request) && !quotedWords(request);
}

namespace {
/// What Apple's model reads on the Mac (RecipeOut), read here with rules on the same words.
LocalRecipe readSettings(const QString& request, QString* mediaAction) {
    const QString t = request.toLower();
    LocalRecipe r;
    // Length: "30s", "45 seconds", "2 minutes", "1:30", "a minute", "half a minute".
    if (auto m = QRegularExpression(QStringLiteral("(\\d+):(\\d\\d)\\b")).match(t); m.hasMatch())
        r.seconds = m.captured(1).toDouble() * 60 + m.captured(2).toDouble();
    else if (auto m2 = QRegularExpression(QStringLiteral("(\\d+(\\.\\d+)?)\\s*(m|min|mins|minute|minutes)\\b")).match(t); m2.hasMatch())
        r.seconds = m2.captured(1).toDouble() * 60;
    else if (auto m3 = QRegularExpression(QStringLiteral("(\\d+(\\.\\d+)?)\\s*(s|sec|secs|second|seconds)\\b")).match(t); m3.hasMatch())
        r.seconds = m3.captured(1).toDouble();
    else if (t.contains(QLatin1String("half a minute"))) r.seconds = 30;
    else if (matches(t, QStringLiteral("\\b(a|one) minute\\b"))) r.seconds = 60;
    if (r.seconds) r.seconds = std::clamp(*r.seconds, 1.0, 900.0);
    // Pace. ("Slow motion" is a speed, not a pace.)
    const QString paceText = QString(t).remove(QRegularExpression(QStringLiteral("slow[- ]?mo(tion)?")));
    if (any(paceText, {"fast", "quick", "punch", "snappy", "energ", "upbeat", "dynamic", "rapid", "hype"})) r.pace = LocalRecipe::Pace::fast;
    else if (any(paceText, {"calm", "slow", "relax", "chill", "gentle", "slower", "longer shots", "peaceful"})) r.pace = LocalRecipe::Pace::calm;
    // A look.
    if (any(t, {"no look", "no filter", "no grade", "remove the look", "take off the look", "remove looks", "without a look", "natural colo"}))
        r.look = QStringLiteral("none");
    else if (any(t, {"black and white", "b&w", "monochrome", "mono", "greyscale", "grayscale"})) r.look = QStringLiteral("Mono");
    else if (any(t, {"teal", "orange and teal"})) r.look = QStringLiteral("Teal and orange");
    else if (any(t, {"golden"})) r.look = QStringLiteral("Golden hour");
    else if (any(t, {"bleach"})) r.look = QStringLiteral("Bleach bypass");
    else if (any(t, {"pastel"})) r.look = QStringLiteral("Pastel");
    else if (any(t, {"faded", "vintage", "retro"})) r.look = QStringLiteral("Faded");
    else if (any(t, {"cool night", "cool", "night", "cold"})) r.look = QStringLiteral("Cool night");
    else if (any(t, {"film print", "cinematic", "movie look"})) r.look = QStringLiteral("Film print");
    else if (any(t, {"warm"})) r.look = QStringLiteral("Warm film");
    // Music.
    if (any(t, {"no music", "without music", "remove the music", "remove music", "mute the music", "music off", "take out the music", "drop the music",
                "no song", "without the song", "silent"}))
        r.music = LocalRecipe::Music::off;
    else if (any(t, {"add music", "add a song", "add some music", "with music", "put music", "put a song", "music under", "add a soundtrack"}))
        r.music = LocalRecipe::Music::add;
    // Frame.
    if (any(t, {"vertical", "portrait", "tiktok", "tik tok", "reel", "shorts", "story", "stories", "9:16", "for phone"})) r.frame = LocalRecipe::Frame::vertical;
    else if (any(t, {"square", "1:1"})) r.frame = LocalRecipe::Frame::square;
    else if (any(t, {"horizontal", "landscape", "widescreen", "youtube", "16:9"})) r.frame = LocalRecipe::Frame::horizontal;
    // Named files: only these, or not these.
    *mediaAction = any(t, {"remove", "take out", "without", "delete", "drop", "get rid", "cut out", "no more", "lose "}) ? QStringLiteral("remove")
                 : any(t, {"only", "just ", "nothing else", "nothing but", "solely"}) ? QStringLiteral("only") : QStringLiteral("unchanged");
    return r;
}

QString without(const QString& cannotDo, std::initializer_list<const char*> about) {
    QStringList kept;
    for (const QString& p : cannotDo.split(u',')) {
        const QString s = p.trimmed();
        if (!s.isEmpty() && !any(s.toLower(), about)) kept << s;
    }
    return kept.join(QStringLiteral(", "));
}
} // namespace

LocalRecipe recipe(const QString& request, const std::vector<MediaAsset>& media, const std::set<Uuid>& pointed, bool hasEdit, const Shows& shows) {
    static const QStringList notThings = {"make", "the", "and", "use", "using", "only", "just", "clip", "clips", "shot", "shots", "video", "videos", "cut",
        "edit", "fast", "slow", "with", "out", "take", "remove", "keep", "all", "some", "more", "less", "seconds", "second", "minute", "music", "song",
        "look", "vertical", "square", "from", "into", "this", "that", "these", "show", "shows", "showing", "see", "want", "give", "put", "add", "where",
        "who", "any", "little", "big"};
    QStringList words;
    for (const QString& w : Transcript::tokens(request)) if (w.size() >= 3 && !notThings.contains(w)) words << w;
    const auto shownFiles = showing(words, media, shows);
    std::vector<MediaAsset> namedFiles;
    for (const auto& a : media) if (pointed.count(a.id) || named(request, {a}).size() == 1) namedFiles.push_back(a);
    for (const auto& a : shownFiles) if (!pointed.count(a.id) && named(request, {a}).size() != 1) namedFiles.push_back(a);

    QString action;
    LocalRecipe r = checked(readSettings(request, &action), request);
    applyMedia(r, namedFiles, action, request, pointed, hasEdit);
    for (const auto& a : shownFiles) r.shown.push_back(a.id);
    if (!shownFiles.empty()) if (auto tags = shows(shownFiles.front().id)) {
        QStringList matched;
        for (const QString& w : words) if (tags->shows(w)) matched << w;
        r.asked = askedPhrase(request, matched);
    }
    r.moments = moments(words, shownFiles, shows);
    // What the Mac's popover adds after its model's reading.
    r.speed = speed(request);
    r.soundEffects = wantsSoundEffects(request);
    r.askedJudgement = asksJudgement(request);
    r.title = titleAsked(request);
    r.titleAtEnd = titleAtEnd(request);
    r.wholeLibrary = wantsLibrary(request);
    r.shots = shotCount(request);
    r.every = wantsEvery(request);
    if (r.every) r.wholeLibrary = true;
    if (r.title) r.cannotDo = without(r.cannotDo, {"title", "text", "caption", "heading", "name"});
    if (captionsWithCut(request)) r.cannotDo = without(r.cannotDo, {"caption", "subtitle"});
    if (wantsLevels(request)) {
        r.levelAudio = true;
        r.cannotDo = without(r.cannotDo, {"level", "volume", "loud", "equaliz", "equalis", "normaliz", "normalis", "balance", "db", "gain", "audio"});
    }
    if (r.soundEffects) r.cannotDo = without(r.cannotDo, {"sound", "sfx", "effect", "whoosh"});
    if (r.speed) r.cannotDo = without(r.cannotDo, {"speed", "fast", "slow", "%"});
    r.onlyAsked = !isGeneral(request);
    return r;
}

} // namespace er::LocalBrain
