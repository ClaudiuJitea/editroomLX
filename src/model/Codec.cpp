#include "Codec.h"
#include "Json.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
#include <stdexcept>

namespace er::codec {

namespace {

struct DecodeError : std::runtime_error { using std::runtime_error::runtime_error; };

// MARK: Enum raw values

template <class E> struct Raw;
#define RAW(E, ...) template <> struct Raw<E> { static constexpr std::pair<E, const char*> table[] = {__VA_ARGS__}; };
RAW(TrackKind, {TrackKind::video, "video"}, {TrackKind::audio, "audio"})
RAW(ClipKind, {ClipKind::video, "video"}, {ClipKind::audio, "audio"}, {ClipKind::image, "image"},
    {ClipKind::solid, "solid"}, {ClipKind::text, "text"})
RAW(MediaKind, {MediaKind::video, "video"}, {MediaKind::audio, "audio"}, {MediaKind::image, "image"})
RAW(FadeCurve, {FadeCurve::linear, "linear"}, {FadeCurve::easeIn, "easeIn"}, {FadeCurve::easeOut, "easeOut"},
    {FadeCurve::smooth, "smooth"})
RAW(TransitionKind, {TransitionKind::dissolve, "dissolve"}, {TransitionKind::dipToBlack, "dipToBlack"},
    {TransitionKind::wipe, "wipe"})
RAW(ZoomMove::Mode, {ZoomMove::Mode::zoomIn, "in"}, {ZoomMove::Mode::inOut, "inOut"}, {ZoomMove::Mode::zoomOut, "out"})
RAW(Marker::Tint, {Marker::Tint::white, "white"}, {Marker::Tint::yellow, "yellow"}, {Marker::Tint::green, "green"},
    {Marker::Tint::blue, "blue"}, {Marker::Tint::purple, "purple"}, {Marker::Tint::red, "red"})
#undef RAW

template <class E> QString rawValue(E e) {
    for (const auto& [v, s] : Raw<E>::table) if (v == e) return QString::fromLatin1(s);
    return {};
}

// MARK: Reader

/// One keyed container. Required keys missing on read take their default (the Mac would refuse the
/// file; LX is lenient there, and always writes them).
class Reader {
public:
    Reader(const QJsonValue& v, QString path) : path_(std::move(path)) {
        if (!v.isObject()) fail(QStringLiteral("expected an object"));
        o_ = v.toObject();
    }
    [[noreturn]] void fail(const QString& why, const QString& key = {}) const {
        throw DecodeError((path_ + (key.isEmpty() ? QString() : u'.' + key) + QStringLiteral(": ") + why).toStdString());
    }
    QString at(const QString& key) const { return path_ + u'.' + key; }
    bool has(const char* k) { known_.insert(QLatin1String(k)); return o_.contains(QLatin1String(k)) && !o_.value(QLatin1String(k)).isNull(); }
    QJsonValue get(const char* k) { known_.insert(QLatin1String(k)); return o_.value(QLatin1String(k)); }

    double num(const char* k, double def) {
        if (!has(k)) return def;
        const QJsonValue v = get(k);
        if (!v.isDouble()) fail(QStringLiteral("expected a number"), QLatin1String(k));
        return v.toDouble();
    }
    std::optional<double> optNum(const char* k) { if (!has(k)) return std::nullopt; return num(k, 0); }
    int integer(const char* k, int def) {
        if (!has(k)) return def;
        const double d = num(k, def);
        if (d != std::floor(d) || std::abs(d) > 2147483647.0) fail(QStringLiteral("expected an integer"), QLatin1String(k));
        return int(d);
    }
    bool boolean(const char* k, bool def) {
        if (!has(k)) return def;
        const QJsonValue v = get(k);
        if (!v.isBool()) fail(QStringLiteral("expected a boolean"), QLatin1String(k));
        return v.toBool();
    }
    std::optional<bool> optBool(const char* k) { if (!has(k)) return std::nullopt; return boolean(k, false); }
    QString str(const char* k, const QString& def = {}) {
        if (!has(k)) return def;
        const QJsonValue v = get(k);
        if (!v.isString()) fail(QStringLiteral("expected a string"), QLatin1String(k));
        return v.toString();
    }
    std::optional<QString> optStr(const char* k) { if (!has(k)) return std::nullopt; return str(k); }
    Uuid uuid(const char* k, bool required = true) {
        if (!has(k)) { if (required) return newUuid(); return {}; }
        auto u = json::parseUuid(str(k));
        if (!u) fail(QStringLiteral("not a UUID"), QLatin1String(k));
        return *u;
    }
    std::optional<Uuid> optUuid(const char* k) { if (!has(k)) return std::nullopt; return uuid(k); }
    std::optional<QByteArray> optData(const char* k) {
        if (!has(k)) return std::nullopt;
        auto r = QByteArray::fromBase64Encoding(str(k).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!r) fail(QStringLiteral("not base64 data"), QLatin1String(k));
        return *r;
    }
    template <class E> E enumeration(const char* k, E def) {
        if (!has(k)) return def;
        const QString s = str(k);
        for (const auto& [v, name] : Raw<E>::table) if (s == QLatin1String(name)) return v;
        fail(QStringLiteral("unknown value \"%1\"").arg(s), QLatin1String(k));
    }
    template <class E> std::optional<E> optEnum(const char* k) { if (!has(k)) return std::nullopt; return enumeration<E>(k, E{}); }
    QJsonArray array(const char* k) {
        const QJsonValue v = get(k);
        if (!v.isArray()) fail(QStringLiteral("expected an array"), QLatin1String(k));
        return v.toArray();
    }

    Extra extras() const {
        Extra e;
        for (auto it = o_.begin(); it != o_.end(); ++it) if (!known_.contains(it.key())) e.insert(it.key(), it.value());
        return e;
    }

private:
    QJsonObject o_;
    QString path_;
    QSet<QString> known_;
};

// MARK: Writer

class Writer {
public:
    explicit Writer(const Extra& extra) : o_(extra) {}
    void set(const char* k, const QJsonValue& v) { o_.insert(QLatin1String(k), v); }
    void set(const char* k, double v) {
        if (!std::isfinite(v)) v = 0; // JSONEncoder throws on non-finite numbers; LX writes 0 instead of failing.
        o_.insert(QLatin1String(k), v);
    }
    void set(const char* k, int v) { o_.insert(QLatin1String(k), v); }
    void set(const char* k, bool v) { o_.insert(QLatin1String(k), v); }
    void set(const char* k, const QString& v) { o_.insert(QLatin1String(k), v); }
    void set(const char* k, const Uuid& v) { o_.insert(QLatin1String(k), json::uuidString(v)); }
    void setData(const char* k, const QByteArray& v) { o_.insert(QLatin1String(k), QString::fromLatin1(v.toBase64())); }
    /// `encodeIfPresent`: nil optionals are left out (and a stale value in `extra` removed).
    template <class T> void opt(const char* k, const std::optional<T>& v) {
        if (v) set(k, *v); else o_.remove(QLatin1String(k));
    }
    QJsonObject done() { return o_; }

private:
    QJsonObject o_;
};

// MARK: Types

QJsonObject enc(const RGBA& c) {
    Writer w(c.extra);
    w.set("r", c.r); w.set("g", c.g); w.set("b", c.b); w.set("a", c.a);
    return w.done();
}
RGBA decRGBA(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    RGBA c;
    c.r = r.num("r", 0); c.g = r.num("g", 0); c.b = r.num("b", 0); c.a = r.num("a", 0);
    c.extra = r.extras();
    return c;
}

QJsonObject enc(const Track& t) {
    Writer w(t.extra);
    w.set("id", t.id); w.set("kind", rawValue(t.kind)); w.set("hidden", t.hidden); w.set("muted", t.muted);
    w.set("gainDB", t.gainDB); w.set("solo", t.solo); w.set("locked", t.locked);
    w.opt("duckUnder", t.duckUnder); w.set("duckDB", t.duckDB);
    return w.done();
}
Track decTrack(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    Track t;
    t.id = r.uuid("id");
    t.kind = r.enumeration("kind", TrackKind::video);
    t.hidden = r.boolean("hidden", false);
    t.muted = r.boolean("muted", false);
    t.gainDB = r.num("gainDB", 0);
    t.solo = r.boolean("solo", false);
    t.locked = r.boolean("locked", false);
    t.duckUnder = r.optUuid("duckUnder");
    t.duckDB = r.num("duckDB", -12);
    t.extra = r.extras();
    return t;
}

QJsonObject enc(const TextStyle& s) {
    Writer w(s.extra);
    w.set("string", s.string); w.set("fontName", s.fontName); w.set("bold", s.bold); w.set("size", s.size);
    w.set("color", enc(s.color)); w.set("x", s.x); w.set("y", s.y);
    return w.done();
}
TextStyle decTextStyle(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    TextStyle s;
    s.string = r.str("string", s.string); s.fontName = r.str("fontName"); s.bold = r.boolean("bold", true);
    s.size = r.num("size", 110);
    if (r.has("color")) s.color = decRGBA(r.get("color"), r.at("color"));
    s.x = r.num("x", 0.5); s.y = r.num("y", 0.5);
    s.extra = r.extras();
    return s;
}

QJsonObject enc(const WheelPoint& p) { Writer w(p.extra); w.set("x", p.x); w.set("y", p.y); return w.done(); }
WheelPoint decWheel(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    WheelPoint p; p.x = r.num("x", 0); p.y = r.num("y", 0); p.extra = r.extras();
    return p;
}

QJsonObject enc(const ColorGrade& g) {
    Writer w(g.extra);
    w.opt("look", g.look); w.set("lookIntensity", g.lookIntensity); w.set("exposure", g.exposure);
    w.set("contrast", g.contrast); w.set("saturation", g.saturation); w.set("temperature", g.temperature);
    w.set("tint", g.tint); w.set("shadows", enc(g.shadows)); w.set("midtones", enc(g.midtones));
    w.set("highlights", enc(g.highlights)); w.set("shadowsLevel", g.shadowsLevel); w.set("midtonesLevel", g.midtonesLevel);
    w.set("highlightsLevel", g.highlightsLevel); w.set("off", g.off);
    return w.done();
}
ColorGrade decGrade(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    ColorGrade g;
    g.look = r.optStr("look"); g.lookIntensity = r.num("lookIntensity", 1); g.exposure = r.num("exposure", 0);
    g.contrast = r.num("contrast", 0); g.saturation = r.num("saturation", 0); g.temperature = r.num("temperature", 0);
    g.tint = r.num("tint", 0);
    if (r.has("shadows")) g.shadows = decWheel(r.get("shadows"), r.at("shadows"));
    if (r.has("midtones")) g.midtones = decWheel(r.get("midtones"), r.at("midtones"));
    if (r.has("highlights")) g.highlights = decWheel(r.get("highlights"), r.at("highlights"));
    // Read field by field, each left at its default when a project doesn't have it (saved before it existed).
    g.shadowsLevel = r.num("shadowsLevel", 0); g.midtonesLevel = r.num("midtonesLevel", 0);
    g.highlightsLevel = r.num("highlightsLevel", 0); g.off = r.boolean("off", false);
    g.extra = r.extras();
    return g;
}

QJsonObject enc(const ZoomMove& z) {
    Writer w(z.extra);
    w.set("mode", rawValue(z.mode)); w.set("p1", z.p1); w.set("p2", z.p2); w.set("p3", z.p3); w.set("p4", z.p4);
    w.set("smooth", z.smooth);
    return w.done();
}
ZoomMove decZoom(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    ZoomMove z;
    z.mode = r.enumeration("mode", ZoomMove::Mode::zoomIn);
    z.p1 = r.num("p1", 0); z.p2 = r.num("p2", 0); z.p3 = r.num("p3", 0); z.p4 = r.num("p4", 0);
    z.smooth = r.boolean("smooth", true);
    z.extra = r.extras();
    return z;
}

QJsonObject enc(const ClipTransform& t) {
    Writer w(t.extra);
    w.set("scale", t.scale); w.set("x", t.x); w.set("y", t.y);
    if (t.zoom) w.set("zoom", enc(*t.zoom)); else w.opt<double>("zoom", std::nullopt);
    return w.done();
}
ClipTransform decTransform(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    ClipTransform t;
    t.scale = r.num("scale", 1); t.x = r.num("x", 0); t.y = r.num("y", 0);
    if (r.has("zoom")) t.zoom = decZoom(r.get("zoom"), r.at("zoom"));
    t.extra = r.extras();
    return t;
}

QJsonObject enc(const Transition& t) {
    Writer w(t.extra);
    w.set("kind", rawValue(t.kind)); w.set("duration", t.duration);
    return w.done();
}
Transition decTransition(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    Transition t;
    t.kind = r.enumeration("kind", TransitionKind::dissolve);
    t.duration = r.num("duration", 0.5);
    t.extra = r.extras();
    return t;
}

QJsonObject enc(const ReverseSource& s) {
    Writer w(s.extra);
    w.set("assetID", s.assetID); w.set("sourceStart", s.sourceStart);
    return w.done();
}
ReverseSource decReverse(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    ReverseSource s;
    s.assetID = r.uuid("assetID"); s.sourceStart = r.num("sourceStart", 0);
    s.extra = r.extras();
    return s;
}

QJsonObject enc(const SpeedRamp& s) {
    Writer w(s.extra);
    w.set("inFrom", s.inFrom); w.set("inTo", s.inTo); w.set("outFrom", s.outFrom); w.set("outTo", s.outTo);
    w.set("startSpeed", s.startSpeed); w.set("endSpeed", s.endSpeed);
    return w.done();
}
SpeedRamp decRamp(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    SpeedRamp s;
    s.inFrom = r.num("inFrom", 0); s.inTo = r.num("inTo", 0); s.outFrom = r.num("outFrom", 0); s.outTo = r.num("outTo", 0);
    s.startSpeed = r.num("startSpeed", 1); s.endSpeed = r.num("endSpeed", 1);
    s.extra = r.extras();
    return s;
}

QJsonObject enc(const Clip& c) {
    Writer w(c.extra);
    w.set("id", c.id); w.set("trackID", c.trackID); w.set("kind", rawValue(c.kind)); w.opt("assetID", c.assetID);
    w.set("start", c.start); w.set("duration", c.duration); w.set("sourceStart", c.sourceStart); w.set("speed", c.speed);
    w.set("fadeIn", c.fadeIn); w.set("fadeOut", c.fadeOut);
    if (c.fadeInCurve) w.set("fadeInCurve", rawValue(*c.fadeInCurve)); else w.opt<double>("fadeInCurve", std::nullopt);
    if (c.fadeOutCurve) w.set("fadeOutCurve", rawValue(*c.fadeOutCurve)); else w.opt<double>("fadeOutCurve", std::nullopt);
    w.set("gainDB", c.gainDB); w.opt("linkID", c.linkID); w.set("color", enc(c.color)); w.set("text", enc(c.text));
    if (c.grade) w.set("grade", enc(*c.grade)); else w.opt<double>("grade", std::nullopt);
    if (c.transform) w.set("transform", enc(*c.transform)); else w.opt<double>("transform", std::nullopt);
    if (c.transitionIn) w.set("transitionIn", enc(*c.transitionIn)); else w.opt<double>("transitionIn", std::nullopt);
    w.opt("freeze", c.freeze);
    if (c.reversedFrom) w.set("reversedFrom", enc(*c.reversedFrom)); else w.opt<double>("reversedFrom", std::nullopt);
    w.opt("reversed", c.reversed);
    if (c.ramp) w.set("ramp", enc(*c.ramp)); else w.opt<double>("ramp", std::nullopt);
    w.opt("captioned", c.captioned);
    return w.done();
}
Clip decClip(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    Clip c;
    c.id = r.uuid("id"); c.trackID = r.uuid("trackID"); c.kind = r.enumeration("kind", ClipKind::video);
    c.assetID = r.optUuid("assetID");
    c.start = r.num("start", 0); c.duration = r.num("duration", 0); c.sourceStart = r.num("sourceStart", 0);
    c.speed = r.num("speed", 1); c.fadeIn = r.num("fadeIn", 0); c.fadeOut = r.num("fadeOut", 0);
    c.fadeInCurve = r.optEnum<FadeCurve>("fadeInCurve"); c.fadeOutCurve = r.optEnum<FadeCurve>("fadeOutCurve");
    c.gainDB = r.num("gainDB", 0); c.linkID = r.optUuid("linkID");
    if (r.has("color")) c.color = decRGBA(r.get("color"), r.at("color"));
    if (r.has("text")) c.text = decTextStyle(r.get("text"), r.at("text"));
    if (r.has("grade")) c.grade = decGrade(r.get("grade"), r.at("grade"));
    if (r.has("transform")) c.transform = decTransform(r.get("transform"), r.at("transform"));
    if (r.has("transitionIn")) c.transitionIn = decTransition(r.get("transitionIn"), r.at("transitionIn"));
    c.freeze = r.optBool("freeze");
    if (r.has("reversedFrom")) c.reversedFrom = decReverse(r.get("reversedFrom"), r.at("reversedFrom"));
    c.reversed = r.optBool("reversed");
    if (r.has("ramp")) c.ramp = decRamp(r.get("ramp"), r.at("ramp"));
    c.captioned = r.optBool("captioned");
    c.extra = r.extras();
    return c;
}

QJsonObject enc(const MediaAsset& a) {
    Writer w(a.extra);
    w.set("id", a.id); w.set("path", a.path); w.opt("relativePath", a.relativePath); w.opt("playablePath", a.playablePath);
    w.set("name", a.name); w.set("kind", rawValue(a.kind)); w.set("duration", a.duration); w.set("width", a.width);
    w.set("height", a.height); w.set("fps", a.fps); w.set("hasAudio", a.hasAudio);
    if (a.bookmark) w.setData("bookmark", *a.bookmark); else w.opt<double>("bookmark", std::nullopt);
    return w.done();
}
MediaAsset decAsset(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    MediaAsset a;
    a.id = r.uuid("id"); a.path = r.str("path"); a.relativePath = r.optStr("relativePath");
    a.playablePath = r.optStr("playablePath"); a.name = r.str("name"); a.kind = r.enumeration("kind", MediaKind::video);
    a.duration = r.num("duration", 0); a.width = r.integer("width", 0); a.height = r.integer("height", 0);
    a.fps = r.num("fps", 0); a.hasAudio = r.boolean("hasAudio", false); a.bookmark = r.optData("bookmark");
    a.extra = r.extras();
    return a;
}

QJsonObject enc(const Marker& m) {
    Writer w(m.extra);
    w.set("id", m.id); w.set("time", m.time); w.set("name", m.name); w.set("tint", rawValue(m.tint));
    return w.done();
}
Marker decMarker(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    Marker m;
    m.id = r.uuid("id"); m.time = r.num("time", 0); m.name = r.str("name"); m.tint = r.enumeration("tint", Marker::Tint::yellow);
    m.extra = r.extras();
    return m;
}

QJsonObject enc(const MagicHistoryEntry& e) {
    Writer w(e.extra);
    w.set("text", e.text); w.set("tab", e.tab); w.set("date", e.date);
    return w.done();
}
MagicHistoryEntry decHistory(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    MagicHistoryEntry e;
    e.text = r.str("text"); e.tab = r.str("tab"); e.date = r.num("date", 0);
    e.extra = r.extras();
    return e;
}

QJsonObject enc(const CaptionStyle& s) {
    Writer w(s.extra);
    w.set("size", s.size); w.set("y", s.y); w.set("highlight", s.highlight); w.set("textColor", enc(s.textColor));
    w.set("highlightColor", enc(s.highlightColor)); w.set("outline", s.outline); w.set("outlineColor", enc(s.outlineColor));
    return w.done();
}
CaptionStyle decCaptionStyle(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    const CaptionStyle d;
    CaptionStyle s;
    s.size = r.num("size", d.size); s.y = r.num("y", d.y); s.highlight = r.boolean("highlight", d.highlight);
    if (r.has("textColor")) s.textColor = decRGBA(r.get("textColor"), r.at("textColor"));
    if (r.has("highlightColor")) s.highlightColor = decRGBA(r.get("highlightColor"), r.at("highlightColor"));
    s.outline = r.boolean("outline", d.outline);
    if (r.has("outlineColor")) s.outlineColor = decRGBA(r.get("outlineColor"), r.at("outlineColor"));
    s.extra = r.extras();
    return s;
}

QJsonObject enc(const CaptionTrack& t) {
    Writer w(t.extra);
    w.set("hidden", t.hidden); w.set("style", enc(t.style));
    return w.done();
}
CaptionTrack decCaptionTrack(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    CaptionTrack t;
    t.hidden = r.boolean("hidden", false);
    if (r.has("style")) t.style = decCaptionStyle(r.get("style"), r.at("style"));
    t.extra = r.extras();
    return t;
}

QJsonObject enc(const CaptionFix& f) {
    Writer w(f.extra);
    w.set("start", f.start); w.set("text", f.text); w.opt("merged", f.merged);
    return w.done();
}
CaptionFix decFix(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    CaptionFix f;
    f.start = r.num("start", 0); f.text = r.str("text"); f.merged = r.optBool("merged");
    f.extra = r.extras();
    return f;
}

QJsonObject enc(const MediaFolder& f) {
    Writer w(f.extra);
    w.set("id", f.id); w.set("path", f.path); w.opt("relativePath", f.relativePath);
    if (f.bookmark) w.setData("bookmark", *f.bookmark); else w.opt<double>("bookmark", std::nullopt);
    return w.done();
}
MediaFolder decFolder(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    MediaFolder f;
    f.id = r.uuid("id"); f.path = r.str("path"); f.relativePath = r.optStr("relativePath"); f.bookmark = r.optData("bookmark");
    f.extra = r.extras();
    return f;
}

template <class T, class F> QJsonArray encList(const std::vector<T>& v, F f) {
    QJsonArray a;
    for (const auto& x : v) a.append(f(x));
    return a;
}
template <class T, class F> std::vector<T> decList(Reader& r, const char* key, F f) {
    std::vector<T> out;
    const QJsonArray a = r.array(key);
    for (qsizetype i = 0; i < a.size(); ++i) out.push_back(f(a[i], r.at(QLatin1String(key)) + QStringLiteral("[%1]").arg(i)));
    return out;
}

QJsonObject encProject(const Project& p) {
    Writer w(p.extra);
    w.set("width", p.width); w.set("height", p.height); w.set("fps", p.fps); w.opt("hdr", p.hdr);
    w.set("settingsLocked", p.settingsLocked);
    w.set("videoTracks", encList(p.videoTracks, [](const Track& t) { return enc(t); }));
    w.set("audioTracks", encList(p.audioTracks, [](const Track& t) { return enc(t); }));
    w.set("clips", encList(p.clips, [](const Clip& c) { return enc(c); }));
    w.set("assets", encList(p.assets, [](const MediaAsset& a) { return enc(a); }));
    w.opt("exportName", p.exportName); w.opt("exportDir", p.exportDir);
    if (p.markers) w.set("markers", encList(*p.markers, [](const Marker& m) { return enc(m); })); else w.opt<double>("markers", std::nullopt);
    w.opt("inPoint", p.inPoint); w.opt("outPoint", p.outPoint);
    if (p.magicHistory) w.set("magicHistory", encList(*p.magicHistory, [](const MagicHistoryEntry& e) { return enc(e); }));
    else w.opt<double>("magicHistory", std::nullopt);
    if (p.captions) w.set("captions", enc(*p.captions)); else w.opt<double>("captions", std::nullopt);
    if (p.captionFixes) {
        // Swift encodes a dictionary whose keys aren't String/Int as a flat [key, value, key, value…]
        // array. Swift's order is the dictionary's (unordered); LX writes them by key for stable output.
        QJsonArray a;
        for (const auto& [id, fixes] : *p.captionFixes) {
            a.append(json::uuidString(id));
            a.append(encList(fixes, [](const CaptionFix& f) { return enc(f); }));
        }
        w.set("captionFixes", a);
    } else {
        w.opt<double>("captionFixes", std::nullopt);
    }
    if (p.folders) w.set("folders", encList(*p.folders, [](const MediaFolder& f) { return enc(f); })); else w.opt<double>("folders", std::nullopt);
    if (p.hiddenMedia) {
        QJsonArray a;
        for (const auto& h : *p.hiddenMedia) a.append(h);
        w.set("hiddenMedia", a);
    } else {
        w.opt<double>("hiddenMedia", std::nullopt);
    }
    return w.done();
}

Project decProject(const QJsonValue& v, const QString& path) {
    Reader r(v, path);
    Project p;
    p.width = r.integer("width", 1920); p.height = r.integer("height", 1080); p.fps = r.num("fps", 30);
    if (r.has("hdr")) p.hdr = r.boolean("hdr", false);
    p.settingsLocked = r.boolean("settingsLocked", false);
    if (r.has("videoTracks")) p.videoTracks = decList<Track>(r, "videoTracks", decTrack);
    if (r.has("audioTracks")) p.audioTracks = decList<Track>(r, "audioTracks", decTrack);
    if (r.has("clips")) p.clips = decList<Clip>(r, "clips", decClip);
    if (r.has("assets")) p.assets = decList<MediaAsset>(r, "assets", decAsset);
    p.exportName = r.optStr("exportName"); p.exportDir = r.optStr("exportDir");
    if (r.has("markers")) p.markers = decList<Marker>(r, "markers", decMarker);
    p.inPoint = r.optNum("inPoint"); p.outPoint = r.optNum("outPoint");
    if (r.has("magicHistory")) p.magicHistory = decList<MagicHistoryEntry>(r, "magicHistory", decHistory);
    if (r.has("captions")) p.captions = decCaptionTrack(r.get("captions"), r.at("captions"));
    if (r.has("captionFixes")) {
        std::map<Uuid, std::vector<CaptionFix>> fixes;
        const QJsonValue raw = r.get("captionFixes");
        auto decFixes = [&](const QJsonValue& list, const QString& at) {
            std::vector<CaptionFix> out;
            if (!list.isArray()) r.fail(QStringLiteral("expected an array"), at);
            const QJsonArray a = list.toArray();
            for (qsizetype i = 0; i < a.size(); ++i) out.push_back(decFix(a[i], r.at(at) + QStringLiteral("[%1]").arg(i)));
            return out;
        };
        if (raw.isArray()) {
            const QJsonArray a = raw.toArray();
            if (a.size() % 2) r.fail(QStringLiteral("expected key/value pairs"), QStringLiteral("captionFixes"));
            for (qsizetype i = 0; i + 1 < a.size(); i += 2) {
                auto id = json::parseUuid(a[i].toString());
                if (!id) r.fail(QStringLiteral("not a UUID"), QStringLiteral("captionFixes"));
                fixes[*id] = decFixes(a[i + 1], QStringLiteral("captionFixes"));
            }
        } else if (raw.isObject()) {
            // Not what Swift writes, but read it anyway.
            const QJsonObject o = raw.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                auto id = json::parseUuid(it.key());
                if (!id) r.fail(QStringLiteral("not a UUID"), QStringLiteral("captionFixes"));
                fixes[*id] = decFixes(it.value(), QStringLiteral("captionFixes"));
            }
        } else {
            r.fail(QStringLiteral("expected an array"), QStringLiteral("captionFixes"));
        }
        p.captionFixes = fixes;
    }
    if (r.has("folders")) p.folders = decList<MediaFolder>(r, "folders", decFolder);
    if (r.has("hiddenMedia")) {
        std::vector<QString> h;
        for (const auto& x : r.array("hiddenMedia")) {
            if (!x.isString()) r.fail(QStringLiteral("expected strings"), QStringLiteral("hiddenMedia"));
            h.push_back(x.toString());
        }
        p.hiddenMedia = h;
    }
    p.extra = r.extras();
    return p;
}

} // namespace

QJsonObject encode(const Project& p) { return encProject(p); }

QJsonObject encode(const SavedDocument& d) {
    Writer w(d.extra);
    w.set("version", d.version);
    w.set("project", encProject(d.project));
    return w.done();
}

std::optional<Project> decodeProject(const QJsonObject& o, QString* error) {
    try {
        return decProject(o, QStringLiteral("project"));
    } catch (const DecodeError& e) {
        if (error) *error = QString::fromStdString(e.what());
        return std::nullopt;
    }
}

Result decodeDocument(const QByteArray& bytes) {
    Result res;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError) { res.error = pe.errorString(); return res; }
    if (!doc.isObject()) { res.error = QStringLiteral("The file isn't an EditroomLX project."); return res; }
    try {
        Reader r(doc.object(), QString());
        SavedDocument d;
        d.version = r.integer("version", 1);
        if (!r.has("project")) r.fail(QStringLiteral("missing"), QStringLiteral("project"));
        d.project = decProject(r.get("project"), QStringLiteral("project"));
        d.extra = r.extras();
        res.document = std::move(d);
    } catch (const DecodeError& e) {
        res.error = QString::fromStdString(e.what());
    }
    return res;
}

QByteArray saveBytes(const SavedDocument& d) { return json::write(encode(d), true); }
QByteArray compactBytes(const SavedDocument& d) { return json::write(encode(d), false); }

QJsonObject encode(const Transcript& t) {
    QJsonObject o;
    QJsonArray words;
    for (const auto& w : t.words) words.append(QJsonObject{{"start", w.start}, {"end", w.end}, {"text", w.text}});
    o.insert(QStringLiteral("words"), words);
    o.insert(QStringLiteral("language"), t.language);
    return o;
}

QJsonObject encode(const ColorGrade& g) { return enc(g); }
ColorGrade decodeGrade(const QJsonObject& o) {
    try { return decGrade(o, QStringLiteral("grade")); } catch (const DecodeError&) { return ColorGrade(); }
}

std::optional<Transcript> decodeTranscript(const QJsonObject& o) {
    try {
        Reader r(o, QStringLiteral("transcript"));
        Transcript t;
        t.language = r.str("language");
        for (const auto& wv : r.array("words")) {
            Reader wr(wv, QStringLiteral("word"));
            t.words.push_back({wr.num("start", 0), wr.num("end", 0), wr.str("text")});
        }
        return t;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

} // namespace er::codec
