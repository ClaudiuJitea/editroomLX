#include "SceneTags.h"
#include <QJsonArray>
#include <algorithm>
#include <set>

namespace er {

void SceneTags::add(const std::vector<std::pair<double, QStringList>>& described) {
    for (const auto& [t, words] : described) {
        const std::set<QString> unique(words.begin(), words.end());
        for (const QString& w : unique) {
            auto it = std::find_if(tags.begin(), tags.end(), [&](const Tag& g) { return g.name == w; });
            if (it != tags.end()) {
                if (std::find(it->times.begin(), it->times.end(), t) == it->times.end()) it->times.push_back(t);
            } else {
                tags.push_back({w, 0.6, {t}});
            }
        }
    }
}

QStringList SceneTags::top(int n) const {
    auto sorted = tags;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Tag& a, const Tag& b) { return a.confidence > b.confidence; });
    QStringList out;
    for (int i = 0; i < n && i < int(sorted.size()); ++i) out << sorted[i].name;
    return out;
}

QString SceneTags::singular(const QString& w) {
    return w.size() > 3 && w.endsWith(u's') && !w.endsWith(QStringLiteral("ss")) ? w.chopped(1) : w;
}

bool SceneTags::sameStem(const QString& a, const QString& b) {
    if (a.size() < 5 || b.size() < 5) return false;
    auto stem = [](QString w) {
        for (const char* end : {"ing", "ter", "ers", "er", "ed", "es", "s"}) {
            const QString e = QLatin1String(end);
            if (w.size() > e.size() + 3 && w.endsWith(e)) { w.chop(e.size()); break; }
        }
        if (w.endsWith(u'e') && w.size() > 4) w.chop(1);
        return w;
    };
    return stem(a) == stem(b);
}

namespace {
QStringList partsOf(const QString& name) {
    QStringList p = name.split(u' ', Qt::SkipEmptyParts);
    const auto& al = SceneTags::aliases();
    if (auto it = al.find(name); it != al.end()) p += it->second;
    return p;
}
} // namespace

bool SceneTags::shows(const QString& word, bool typing) const {
    const QString w = singular(word.toLower());
    if (w.size() < 2) return false;
    return std::any_of(tags.begin(), tags.end(), [&](const Tag& tag) {
        for (const QString& p : partsOf(tag.name))
            if (p == w || singular(p) == w || (typing && p.startsWith(w)) || sameStem(p, w)) return true;
        return false;
    });
}

std::vector<SceneTags::Tag> SceneTags::matching(const QString& word) const {
    const QString w = singular(word.toLower());
    std::vector<Tag> out;
    if (w.size() < 2) return out;
    for (const auto& tag : tags)
        for (const QString& p : partsOf(tag.name))
            if (p == w || singular(p) == w || sameStem(p, w)) { out.push_back(tag); break; }
    return out;
}

const std::map<QString, QStringList>& SceneTags::aliases() {
    static const std::map<QString, QStringList> a = {
        {QStringLiteral("portal"), {QStringLiteral("door"), QStringLiteral("doorway"), QStringLiteral("entrance"), QStringLiteral("gate")}},
        {QStringLiteral("conveyance"), {QStringLiteral("vehicle"), QStringLiteral("transport")}},
        {QStringLiteral("people"), {QStringLiteral("person"), QStringLiteral("man"), QStringLiteral("woman"), QStringLiteral("guy"), QStringLiteral("girl"), QStringLiteral("friends"), QStringLiteral("crowd")}},
        {QStringLiteral("adult"), {QStringLiteral("person"), QStringLiteral("man"), QStringLiteral("woman"), QStringLiteral("guy"), QStringLiteral("girl")}},
        {QStringLiteral("child"), {QStringLiteral("kid"), QStringLiteral("kids"), QStringLiteral("boy"), QStringLiteral("girl"), QStringLiteral("baby")}},
        {QStringLiteral("structure"), {QStringLiteral("building"), QStringLiteral("architecture")}},
        {QStringLiteral("housing"), {QStringLiteral("house"), QStringLiteral("home"), QStringLiteral("building")}},
        {QStringLiteral("building"), {QStringLiteral("house")}},
        {QStringLiteral("sky"), {QStringLiteral("clouds")}},
        {QStringLiteral("water body"), {QStringLiteral("water"), QStringLiteral("lake"), QStringLiteral("sea"), QStringLiteral("ocean")}},
        {QStringLiteral("swimming pool"), {QStringLiteral("pool"), QStringLiteral("swim")}},
        {QStringLiteral("automobile"), {QStringLiteral("car"), QStringLiteral("vehicle")}},
        {QStringLiteral("vehicle"), {QStringLiteral("car")}},
        {QStringLiteral("aircraft"), {QStringLiteral("plane"), QStringLiteral("airplane"), QStringLiteral("helicopter"), QStringLiteral("vehicle")}},
        {QStringLiteral("rotorcraft"), {QStringLiteral("helicopter"), QStringLiteral("vehicle")}},
        {QStringLiteral("foliage"), {QStringLiteral("plants"), QStringLiteral("leaves"), QStringLiteral("trees")}},
        {QStringLiteral("plant"), {QStringLiteral("plants"), QStringLiteral("greenery")}},
        {QStringLiteral("food"), {QStringLiteral("meal"), QStringLiteral("eating"), QStringLiteral("dinner"), QStringLiteral("lunch"), QStringLiteral("breakfast")}},
        {QStringLiteral("drink"), {QStringLiteral("drinks"), QStringLiteral("cocktail"), QStringLiteral("bar")}},
        {QStringLiteral("nightclub"), {QStringLiteral("club"), QStringLiteral("party")}},
        {QStringLiteral("concert"), {QStringLiteral("gig"), QStringLiteral("show"), QStringLiteral("music"), QStringLiteral("festival")}},
        {QStringLiteral("selfie"), {QStringLiteral("vlog"), QStringLiteral("talking")}},
        {QStringLiteral("sunset sunrise"), {QStringLiteral("sunset"), QStringLiteral("sunrise"), QStringLiteral("golden hour")}},
        {QStringLiteral("snow"), {QStringLiteral("winter")}},
        {QStringLiteral("beach"), {QStringLiteral("sand"), QStringLiteral("ocean"), QStringLiteral("sea")}},
        {QStringLiteral("dog"), {QStringLiteral("puppy")}},
        {QStringLiteral("cat"), {QStringLiteral("kitten")}},
        {QStringLiteral("rollercoaster"), {QStringLiteral("ride"), QStringLiteral("roller"), QStringLiteral("coaster"), QStringLiteral("theme park"), QStringLiteral("amusement")}},
        {QStringLiteral("carousel"), {QStringLiteral("ride"), QStringLiteral("merry")}},
        {QStringLiteral("ferris wheel"), {QStringLiteral("ride")}},
        {QStringLiteral("amusement park"), {QStringLiteral("ride"), QStringLiteral("theme"), QStringLiteral("fair"), QStringLiteral("funfair"), QStringLiteral("carnival"), QStringLiteral("amusement")}},
        {QStringLiteral("fairground"), {QStringLiteral("ride"), QStringLiteral("fair"), QStringLiteral("funfair"), QStringLiteral("carnival")}},
        {QStringLiteral("go kart"), {QStringLiteral("ride"), QStringLiteral("karting"), QStringLiteral("racing"), QStringLiteral("vehicle")}},
        {QStringLiteral("trampoline"), {QStringLiteral("jumping"), QStringLiteral("bounce")}},
        {QStringLiteral("celebration"), {QStringLiteral("party"), QStringLiteral("birthday"), QStringLiteral("celebrate"), QStringLiteral("festival")}},
        {QStringLiteral("cake"), {QStringLiteral("birthday"), QStringLiteral("dessert")}},
        {QStringLiteral("fireworks"), {QStringLiteral("celebration"), QStringLiteral("festival")}},
        {QStringLiteral("wedding"), {QStringLiteral("marriage"), QStringLiteral("ceremony")}},
        {QStringLiteral("bride"), {QStringLiteral("wedding")}},
        {QStringLiteral("graduation"), {QStringLiteral("ceremony")}},
        {QStringLiteral("crowd"), {QStringLiteral("audience"), QStringLiteral("festival")}},
        {QStringLiteral("dancing"), {QStringLiteral("dance"), QStringLiteral("dancer"), QStringLiteral("party")}},
        {QStringLiteral("breakdancing"), {QStringLiteral("dance"), QStringLiteral("dancing")}},
        {QStringLiteral("ballet dancer"), {QStringLiteral("dance"), QStringLiteral("dancing")}},
        {QStringLiteral("karaoke"), {QStringLiteral("singing"), QStringLiteral("sing")}},
        {QStringLiteral("microphone"), {QStringLiteral("singing"), QStringLiteral("speech"), QStringLiteral("podcast")}},
        {QStringLiteral("musical instrument"), {QStringLiteral("music"), QStringLiteral("band")}},
        {QStringLiteral("guitar"), {QStringLiteral("music"), QStringLiteral("band")}},
        {QStringLiteral("piano"), {QStringLiteral("music")}},
        {QStringLiteral("drum"), {QStringLiteral("music"), QStringLiteral("band")}},
        {QStringLiteral("sport"), {QStringLiteral("sports"), QStringLiteral("game"), QStringLiteral("match")}},
        {QStringLiteral("soccer"), {QStringLiteral("football"), QStringLiteral("sports"), QStringLiteral("game")}},
        {QStringLiteral("football"), {QStringLiteral("sports"), QStringLiteral("game")}},
        {QStringLiteral("basketball"), {QStringLiteral("sports"), QStringLiteral("game")}},
        {QStringLiteral("tennis"), {QStringLiteral("sports")}},
        {QStringLiteral("golf"), {QStringLiteral("sports")}},
        {QStringLiteral("stadium"), {QStringLiteral("arena"), QStringLiteral("game"), QStringLiteral("match")}},
        {QStringLiteral("skateboarding"), {QStringLiteral("skate"), QStringLiteral("skating"), QStringLiteral("skateboard")}},
        {QStringLiteral("cycling"), {QStringLiteral("bike"), QStringLiteral("biking"), QStringLiteral("bicycle"), QStringLiteral("riding")}},
        {QStringLiteral("bicycle"), {QStringLiteral("bike"), QStringLiteral("vehicle")}},
        {QStringLiteral("motorcycle"), {QStringLiteral("motorbike"), QStringLiteral("bike"), QStringLiteral("riding"), QStringLiteral("vehicle")}},
        {QStringLiteral("hiking"), {QStringLiteral("hike"), QStringLiteral("trail"), QStringLiteral("walk")}},
        {QStringLiteral("mountain"), {QStringLiteral("hills")}},
        {QStringLiteral("forest"), {QStringLiteral("woods"), QStringLiteral("trees")}},
        {QStringLiteral("park"), {QStringLiteral("outdoors")}},
        {QStringLiteral("cityscape"), {QStringLiteral("city"), QStringLiteral("skyline"), QStringLiteral("downtown"), QStringLiteral("urban")}},
        {QStringLiteral("skyscraper"), {QStringLiteral("city"), QStringLiteral("tower"), QStringLiteral("downtown")}},
        {QStringLiteral("street"), {QStringLiteral("city"), QStringLiteral("road")}},
        {QStringLiteral("night sky"), {QStringLiteral("stars")}},
        {QStringLiteral("waterfall"), {QStringLiteral("falls")}},
        {QStringLiteral("ocean"), {QStringLiteral("sea"), QStringLiteral("water")}},
        {QStringLiteral("lake"), {QStringLiteral("water")}},
        {QStringLiteral("river"), {QStringLiteral("water"), QStringLiteral("stream")}},
        {QStringLiteral("surfing"), {QStringLiteral("surf"), QStringLiteral("waves")}},
        {QStringLiteral("diving"), {QStringLiteral("scuba"), QStringLiteral("underwater")}},
        {QStringLiteral("snorkeling"), {QStringLiteral("snorkel"), QStringLiteral("underwater")}},
        {QStringLiteral("swimming"), {QStringLiteral("swim")}},
        {QStringLiteral("skiing"), {QStringLiteral("ski"), QStringLiteral("winter")}},
        {QStringLiteral("snowboarding"), {QStringLiteral("snowboard"), QStringLiteral("snow"), QStringLiteral("winter")}},
        {QStringLiteral("jetski"), {QStringLiteral("jet"), QStringLiteral("water"), QStringLiteral("vehicle")}},
        {QStringLiteral("boat"), {QStringLiteral("boating"), QStringLiteral("sailing"), QStringLiteral("vehicle")}},
        {QStringLiteral("sailboat"), {QStringLiteral("sailing"), QStringLiteral("boat"), QStringLiteral("vehicle")}},
        {QStringLiteral("yacht"), {QStringLiteral("boat"), QStringLiteral("sailing"), QStringLiteral("vehicle")}},
        {QStringLiteral("cruise ship"), {QStringLiteral("cruise"), QStringLiteral("boat"), QStringLiteral("vehicle")}},
        {QStringLiteral("kayak"), {QStringLiteral("kayaking"), QStringLiteral("paddling"), QStringLiteral("vehicle")}},
        {QStringLiteral("canoe"), {QStringLiteral("paddling"), QStringLiteral("boat"), QStringLiteral("vehicle")}},
        {QStringLiteral("camping"), {QStringLiteral("camp"), QStringLiteral("tent"), QStringLiteral("outdoors")}},
        {QStringLiteral("tent"), {QStringLiteral("camping")}},
        {QStringLiteral("fire"), {QStringLiteral("campfire"), QStringLiteral("bonfire")}},
        {QStringLiteral("airplane"), {QStringLiteral("plane"), QStringLiteral("flight"), QStringLiteral("flying"), QStringLiteral("vehicle")}},
        {QStringLiteral("airport"), {QStringLiteral("flight"), QStringLiteral("travel")}},
        {QStringLiteral("train"), {QStringLiteral("railway"), QStringLiteral("travel"), QStringLiteral("vehicle")}},
        {QStringLiteral("restaurant"), {QStringLiteral("dinner"), QStringLiteral("eating"), QStringLiteral("dining")}},
        {QStringLiteral("coffee"), {QStringLiteral("cafe")}},
        {QStringLiteral("cocktail"), {QStringLiteral("drinks"), QStringLiteral("bar")}},
        {QStringLiteral("wine"), {QStringLiteral("drinks")}},
        {QStringLiteral("beer"), {QStringLiteral("drinks"), QStringLiteral("bar")}},
        {QStringLiteral("museum"), {QStringLiteral("gallery"), QStringLiteral("art")}},
        {QStringLiteral("painting"), {QStringLiteral("art")}},
        {QStringLiteral("monument"), {QStringLiteral("landmark"), QStringLiteral("sightseeing")}},
        {QStringLiteral("statue"), {QStringLiteral("landmark"), QStringLiteral("sculpture")}},
        {QStringLiteral("castle"), {QStringLiteral("landmark")}},
        {QStringLiteral("ruins"), {QStringLiteral("landmark"), QStringLiteral("history")}},
        {QStringLiteral("zoo"), {QStringLiteral("animals")}},
        {QStringLiteral("aquarium"), {QStringLiteral("fish"), QStringLiteral("animals")}},
        {QStringLiteral("horse"), {QStringLiteral("riding")}},
        {QStringLiteral("baby"), {QStringLiteral("newborn"), QStringLiteral("infant")}},
        {QStringLiteral("teen"), {QStringLiteral("teenager")}},
        {QStringLiteral("yoga"), {QStringLiteral("workout"), QStringLiteral("exercise"), QStringLiteral("stretching")}},
        {QStringLiteral("screenshot"), {QStringLiteral("screen"), QStringLiteral("recording")}},
        {QStringLiteral("laptop"), {QStringLiteral("computer"), QStringLiteral("screen")}},
        {QStringLiteral("playground"), {QStringLiteral("kids")}},
        {QStringLiteral("garden"), {QStringLiteral("flowers"), QStringLiteral("plants")}},
        {QStringLiteral("car"), {QStringLiteral("vehicle")}},
        {QStringLiteral("laughter"), {QStringLiteral("laughing"), QStringLiteral("laugh"), QStringLiteral("funny")}},
        {QStringLiteral("smiles"), {QStringLiteral("smiling"), QStringLiteral("smile"), QStringLiteral("happy")}},
        {QStringLiteral("smile"), {QStringLiteral("smiling"), QStringLiteral("happy")}},
        {QStringLiteral("excitement"), {QStringLiteral("excited"), QStringLiteral("happy")}},
        {QStringLiteral("fun"), {QStringLiteral("happy"), QStringLiteral("funny")}},
        {QStringLiteral("bus"), {QStringLiteral("vehicle")}},
        {QStringLiteral("truck"), {QStringLiteral("vehicle")}},
        {QStringLiteral("semi truck"), {QStringLiteral("vehicle")}},
        {QStringLiteral("police car"), {QStringLiteral("vehicle")}},
        {QStringLiteral("formula one car"), {QStringLiteral("vehicle")}},
        {QStringLiteral("helicopter"), {QStringLiteral("vehicle")}},
    };
    return a;
}

QJsonObject SceneTags::json() const {
    QJsonArray list;
    for (const auto& t : tags) {
        QJsonArray times;
        for (double x : t.times) times.append(x);
        list.append(QJsonObject{{"name", t.name}, {"confidence", t.confidence}, {"times", times}});
    }
    return QJsonObject{{"tags", list}};
}

std::optional<SceneTags> SceneTags::fromJson(const QJsonObject& o) {
    if (!o.value("tags").isArray()) return std::nullopt;
    SceneTags s;
    for (const auto& v : o.value("tags").toArray()) {
        const QJsonObject t = v.toObject();
        if (!t.value("name").isString()) return std::nullopt;
        Tag g{t.value("name").toString(), t.value("confidence").toDouble(), {}};
        for (const auto& x : t.value("times").toArray()) g.times.push_back(x.toDouble());
        s.tags.push_back(std::move(g));
    }
    return s;
}

} // namespace er
