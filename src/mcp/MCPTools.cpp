#include "MCPTools.h"
#include "LocalMagic.h"
#include "MagicShow.h"
#include <QTimer>
#include "model/Captions.h"
#include "analysis/MediaAnalyzer.h"
#include "audio/Waveform.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include "model/Json.h"
#include "model/MediaFolders.h"
#include "rendering/CpuRenderer.h"
#include "ui/Thumbnails.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QEventLoop>
#include <set>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace er {

// MARK: Notes

namespace {
QString notesFolder() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Notes");
    QDir().mkpath(d);
    return d;
}
QString noteFile(const QString& path) { return notesFolder() + u'/' + WaveformReader::cacheName(path) + QStringLiteral(".json"); }
} // namespace

std::optional<MediaNotes::Note> MediaNotes::note(const QString& path) {
    QFile f(noteFile(path));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.contains(QStringLiteral("text"))) return std::nullopt;
    return Note{o.value("text").toString(), o.value("by").toString(), o.value("date").toDouble()};
}

void MediaNotes::save(const QString& text, const QString& path, const QString& by) {
    const QString t = text.trimmed();
    if (t.isEmpty()) return;
    QJsonObject o{{"text", t.left(maxLength)}, {"date", double(QDateTime::currentMSecsSinceEpoch()) / 1000.0 - 978307200.0}};
    if (!by.isEmpty()) o.insert(QStringLiteral("by"), by);
    QSaveFile f(noteFile(path));
    if (f.open(QIODevice::WriteOnly)) { f.write(json::write(o, false)); f.commit(); }
}

// MARK: Drafts

namespace {
std::map<QString, std::unique_ptr<MCPDraft>>& drafts() { static std::map<QString, std::unique_ptr<MCPDraft>> d; return d; }
}

MCPDraft* MCPDraft::open(const Project& p) {
    auto d = std::make_unique<MCPDraft>();
    d->token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->project = p;
    MCPDraft* raw = d.get();
    drafts()[raw->token] = std::move(d);
    return raw;
}

MCPDraft* MCPDraft::find(const QString& token) {
    auto it = drafts().find(token);
    return it == drafts().end() ? nullptr : it->second.get();
}

void MCPDraft::close(const QString& token) { drafts().erase(token); }

void MCPDraft::apply(const std::vector<MagicEdit>& e) {
    undoStack.push_back(project);
    undoEdits.push_back(edits.size());
    redoStack.clear();
    redoEdits.clear();
    for (const auto& x : e) { x.apply(project); edits.push_back(x); }
    project.sanitize();
}

void MCPDraft::undo() {
    if (undoStack.empty()) return;
    redoStack.push_back(project);
    redoEdits.push_back(edits.size());
    project = undoStack.back();
    undoStack.pop_back();
    const size_t keep = undoEdits.back();
    undoEdits.pop_back();
    redone.assign(edits.begin() + qsizetype(keep), edits.end());
    edits.resize(keep);
}

void MCPDraft::redo() {
    if (redoStack.empty()) return;
    undoStack.push_back(project);
    undoEdits.push_back(edits.size());
    project = redoStack.back();
    redoStack.pop_back();
    redoEdits.pop_back();
    edits.insert(edits.end(), redone.begin(), redone.end());
    redone.clear();
}

// MARK: Definitions

namespace {
/// The Mac's words, with the app's Linux name: "Editroom" becomes "EditroomLX" (ids such as "editroom" stay).
static QByteArray branded(QByteArray json) {
    static const QRegularExpression word(QStringLiteral("(?<![\\w/-])Editroom(?![\\w/]| LX)"));
    return QString::fromUtf8(json).replace(word, QStringLiteral("EditroomLX")).replace(QStringLiteral("this Mac"), QStringLiteral("this computer")).toUtf8();
}

QJsonObject loadDefinitions() {
    for (const QString& p : {QCoreApplication::applicationDirPath() + QStringLiteral("/../share/editroom/mcp-tools.json"),
                             QStringLiteral(ER_RESOURCES "/mcp-tools.json")}) {
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) return QJsonDocument::fromJson(branded(f.readAll())).object();
    }
    return {};
}
const QJsonObject& defs() { static const QJsonObject d = loadDefinitions(); return d; }
} // namespace

const QJsonArray& MCPTools::definitions() { static const QJsonArray a = defs().value("tools").toArray(); return a; }
const QString& MCPTools::instructions() { static const QString s = defs().value("instructions").toString(); return s; }

QJsonObject MCPTools::failure(const QString& message) {
    return QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}}}, {"isError", true}};
}

QColor MCPTools::color(const QString& name) {
    if (name == QLatin1String("Claude")) return QColor::fromRgbF(0.85f, 0.47f, 0.34f);
    if (name == QLatin1String("Codex") || name == QLatin1String("ChatGPT")) return QColor::fromRgbF(0.06f, 0.64f, 0.50f);
    uint64_t h = 1469598103934665603ull;
    for (char b : name.toUtf8()) h = (h ^ uint8_t(b)) * 1099511628211ull;
    return QColor::fromHsvF(float(h % 360) / 360, 0.65f, 0.95f);
}

// MARK: JSON

namespace {
/// Three decimal places, written as such ("5.417").
double r3(double v) { return QString::asprintf("%.3f", v).toDouble(); }

QString hex(const RGBA& c) {
    return QString::asprintf("#%02X%02X%02X", int(std::round(c.r * 255)), int(std::round(c.g * 255)), int(std::round(c.b * 255)));
}

QString uuid(const Uuid& u) { return json::uuidString(u); }

QJsonObject assetJSON(const MediaAsset& a) {
    QJsonObject d{{"id", uuid(a.id)}, {"name", a.name}, {"kind", a.kind == MediaKind::video ? "video" : a.kind == MediaKind::audio ? "audio" : "image"}, {"path", a.path}};
    if (a.kind != MediaKind::image) d.insert("duration", r3(a.duration));
    if (a.kind != MediaKind::audio) { d.insert("width", a.width); d.insert("height", a.height); }
    if (a.kind == MediaKind::video) { d.insert("fps", r3(a.fps)); d.insert("has_audio", a.hasAudio); }
    return d;
}

QJsonObject assetJSON(const MediaAsset& a, Controller& c, const std::set<QString>& common = {}) {
    QJsonObject d = assetJSON(a);
    if (auto s = c.sceneTags(a.id)) {
        QJsonArray shows;
        for (const QString& t : s->top(20)) if (!common.count(t) && shows.size() < 8) shows.append(t);
        if (!shows.isEmpty()) d.insert("shows", shows);
    }
    if (auto n = MediaNotes::note(a.path)) d.insert("notes", n->text);
    if (auto t = c.media().transcript(a.id)) {
        d.insert("speech", !t->isEmpty());
        if (!t->isEmpty()) d.insert("says", t->text().left(120) + (t->text().size() > 120 ? QStringLiteral("…") : QString()));
    }
    return d;
}

QJsonArray clipsJSON(const std::vector<Uuid>& ids, const Project& p) {
    QJsonArray a;
    for (const auto& id : ids) if (const Clip* c = p.clip(id)) a.append(MCPTools::clipJSON(*c, p));
    return a;
}

QString kindName(ClipKind k) {
    switch (k) {
    case ClipKind::video: return QStringLiteral("video");
    case ClipKind::audio: return QStringLiteral("audio");
    case ClipKind::image: return QStringLiteral("image");
    case ClipKind::solid: return QStringLiteral("solid");
    case ClipKind::text: return QStringLiteral("text");
    }
    return {};
}

QString tintName(Marker::Tint t) {
    static const char* names[] = {"white", "yellow", "green", "blue", "purple", "red"};
    return QString::fromLatin1(names[int(t)]);
}

QString transitionName(TransitionKind k) {
    return k == TransitionKind::dissolve ? QStringLiteral("dissolve") : k == TransitionKind::dipToBlack ? QStringLiteral("dipToBlack") : QStringLiteral("wipe");
}
} // namespace

QString MCPTools::trackName(const Uuid& id, const Project& p) {
    if (auto i = p.trackIndex(id, TrackKind::video)) return QStringLiteral("V%1").arg(*i + 1);
    if (auto i = p.trackIndex(id, TrackKind::audio)) return QStringLiteral("A%1").arg(*i + 1);
    return QStringLiteral("?");
}

QJsonObject MCPTools::clipJSON(const Clip& c, const Project& p) {
    QJsonObject d{{"id", uuid(c.id)}, {"name", p.name(c)}, {"kind", kindName(c.kind)}, {"track", trackName(c.trackID, p)},
                  {"start", r3(c.start)}, {"end", r3(c.end())}, {"duration", r3(c.duration)}};
    if (usesSource(c.kind)) { d.insert("source_start", r3(c.sourceStart)); d.insert("asset_id", c.assetID ? uuid(*c.assetID) : QString()); }
    if (c.speed != 1) d.insert("speed", c.speed);
    if (c.isRamped()) {
        const SpeedRamp& r = *c.ramp;
        d.insert("speed_ramp", QJsonObject{{"from", r.inTo}, {"to", r.outFrom}, {"ease_in", r.inTo - r.inFrom}, {"ease_out", r.outTo - r.outFrom},
                                           {"start_speed", r.startSpeed}, {"end_speed", r.endSpeed}});
    }
    if (c.isReversed()) d.insert("reversed", true);
    if (c.isFreeze()) d.insert("freeze_frame", true);
    if (c.linkID)
        for (const auto& o : p.clips) if (o.linkID == c.linkID && o.id != c.id) { d.insert("linked_to", uuid(o.id)); break; }
    if (c.kind == ClipKind::audio || c.gainDB != 0) d.insert("gain_db", r3(c.gainDB));
    if (c.fadeIn > 0) d.insert("fade_in", r3(c.fadeIn));
    if (c.fadeOut > 0) d.insert("fade_out", r3(c.fadeOut));
    if (c.transitionIn) d.insert("transition_in", QJsonObject{{"kind", transitionName(c.transitionIn->kind)}, {"duration", c.transitionIn->duration}});
    if (c.kind == ClipKind::text) {
        d.insert("text", c.text.string);
        d.insert("style", QJsonObject{{"font", c.text.fontName.isEmpty() ? QStringLiteral("System") : c.text.fontName}, {"size", r3(c.text.size)},
                                      {"bold", c.text.bold}, {"color", hex(c.text.color)}, {"x", r3(c.text.x)}, {"y", r3(c.text.y)}});
    }
    if (c.kind == ClipKind::solid) d.insert("color", hex(c.color));
    if (c.transform && !c.transform->isIdentity()) {
        QJsonObject t{{"scale", r3(c.transform->scale)}, {"x", r3(c.transform->x)}, {"y", r3(c.transform->y)}};
        // An animated zoom: seconds into the clip that it moves in and out.
        if (const auto& z = c.transform->zoom) {
            QJsonObject zd{{"mode", z->mode == ZoomMove::Mode::zoomIn ? QStringLiteral("in") : z->mode == ZoomMove::Mode::zoomOut ? QStringLiteral("out") : QStringLiteral("in_out")},
                           {"smooth", z->smooth}};
            if (z->mode != ZoomMove::Mode::zoomOut) zd.insert("in", QJsonArray{r3(z->p1), r3(z->p2)});
            if (z->mode != ZoomMove::Mode::zoomIn) zd.insert("out", QJsonArray{r3(z->p3), r3(z->p4)});
            t.insert("zoom", zd);
        }
        d.insert("transform", t);
    }
    if (c.grade) {
        const ColorGrade& g = *c.grade;
        QJsonObject gd;
        if (g.look) { gd.insert("look", g.lookLabel()); if (*g.look != g.lookLabel()) gd.insert("look_file", *g.look); }
        if (g.lookIntensity != 1) gd.insert("look_intensity", g.lookIntensity);
        const std::pair<const char*, double> fields[] = {{"exposure", g.exposure}, {"contrast", g.contrast}, {"saturation", g.saturation},
                                                         {"temperature", g.temperature}, {"tint", g.tint}};
        for (const auto& [k, v] : fields) if (v != 0) gd.insert(QLatin1String(k), r3(v));
        d.insert("grade", gd);
    }
    return d;
}

QJsonObject MCPTools::projectJSON(const Project& p, const QString& title, double playhead) {
    QJsonArray tracks;
    for (int i = int(p.videoTracks.size()) - 1; i >= 0; --i) {
        const Track& t = p.videoTracks[size_t(i)];
        tracks.append(QJsonObject{{"name", QStringLiteral("V%1").arg(i + 1)}, {"kind", "video"}, {"hidden", t.hidden}, {"locked", t.locked}});
    }
    for (size_t i = 0; i < p.audioTracks.size(); ++i) {
        const Track& t = p.audioTracks[i];
        tracks.append(QJsonObject{{"name", QStringLiteral("A%1").arg(i + 1)}, {"kind", "audio"}, {"muted", t.muted}, {"solo", t.solo},
                                  {"locked", t.locked}, {"gain_db", t.gainDB}});
    }
    std::vector<const Clip*> sorted;
    for (const auto& c : p.clips) sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Clip* a, const Clip* b) {
        return std::make_pair(trackName(a->trackID, p), a->start) < std::make_pair(trackName(b->trackID, p), b->start);
    });
    QJsonArray clips;
    for (auto* c : sorted) clips.append(clipJSON(*c, p));
    QJsonArray markers;
    for (const auto& m : p.markerList()) markers.append(QJsonObject{{"id", uuid(m.id)}, {"time", r3(m.time)}, {"name", m.name}, {"color", tintName(m.tint)}});
    QJsonObject out{{"name", title},
                    {"sequence", QJsonObject{{"width", p.width}, {"height", p.height}, {"fps", p.fps}, {"duration", r3(p.duration())}}},
                    {"playhead", r3(playhead)}, {"tracks", tracks}, {"clips", clips}, {"markers", markers}};
    if (auto r = p.playRange()) out.insert("in_out", QJsonObject{{"in", r3(r->start)}, {"out", r3(r->end)}});
    else out.insert("in_out", QJsonValue::Null);
    if (p.captions) {
        QJsonArray ids;
        for (const auto& c : p.captionedClips()) ids.append(uuid(c.id));
        out.insert("captions", QJsonObject{{"hidden", p.captions->hidden}, {"size", p.captions->style.size}, {"position", p.captions->style.y},
                                           {"highlight", p.captions->style.highlight}, {"clip_ids", ids}});
    } else {
        out.insert("captions", QJsonValue::Null);
    }
    return out;
}

// MARK: Calls

namespace {

QJsonArray text(const QString& s) { return QJsonArray{QJsonObject{{"type", "text"}, {"text", s}}}; }
QJsonArray jsonOut(const QJsonValue& v) { return text(QString::fromUtf8(json::write(v, true))); }

std::optional<double> number(const QJsonObject& a, const char* k) {
    const QJsonValue v = a.value(QLatin1String(k));
    if (v.isDouble()) return v.toDouble();
    return std::nullopt;
}

double numArg(const QJsonObject& a, const char* k) {
    if (auto v = number(a, k)) return *v;
    throw MCPToolError{QStringLiteral("%1 is needed, in seconds.").arg(QLatin1String(k))};
}

const MediaAsset& assetArg(const QJsonObject& args, const Project& p) {
    if (!args.value("asset_id").isString()) throw MCPToolError{QStringLiteral("asset_id is needed (see list_media).")};
    const QString s = args.value("asset_id").toString();
    for (const auto& a : p.assets) if (uuid(a.id) == s.toUpper() || a.name == s) return a;
    throw MCPToolError{QStringLiteral("No library item with id %1. list_media has them.").arg(s)};
}

const Clip& clipArg(const QJsonObject& args, const Project& p) {
    if (!args.value("clip_id").isString()) throw MCPToolError{QStringLiteral("clip_id is needed (see get_project).")};
    const QString s = args.value("clip_id").toString();
    auto id = json::parseUuid(s);
    const Clip* c = id ? p.clip(*id) : nullptr;
    if (!c) throw MCPToolError{QStringLiteral("No clip with id %1. get_project has them.").arg(s)};
    if (p.isLocked(c->trackID)) throw MCPToolError{QStringLiteral("That clip is on a locked track.")};
    return *c;
}

Uuid trackArg(const QString& name, const Project& p, TrackKind kind) {
    const QString n = name.toUpper().trimmed();
    const QString prefix = kind == TrackKind::video ? QStringLiteral("V") : QStringLiteral("A");
    bool ok = false;
    const int i = n.mid(1).toInt(&ok);
    if (!n.startsWith(prefix) || !ok || i < 1)
        throw MCPToolError{QStringLiteral("%1 isn't a %2.").arg(name, kind == TrackKind::video ? QStringLiteral("video track (V1, V2…)") : QStringLiteral("audio track (A1, A2…)"))};
    const auto& tracks = p.tracks(kind);
    if (i > int(tracks.size())) throw MCPToolError{QStringLiteral("There's no %1; there are %2. add_track adds one.").arg(n).arg(tracks.size())};
    if (tracks[size_t(i - 1)].locked) throw MCPToolError{QStringLiteral("%1 is locked.").arg(n)};
    return tracks[size_t(i - 1)].id;
}

std::optional<RGBA> colorArg(const QJsonValue& v) {
    if (!v.isString()) return std::nullopt;
    QString h = v.toString().trimmed();
    if (h.startsWith(u'#')) h.remove(0, 1);
    bool ok = false;
    const uint n = h.toUInt(&ok, 16);
    if (h.size() != 6 || !ok) throw MCPToolError{QStringLiteral("Colours are hex, like #FF4A1C.")};
    return RGBA{double((n >> 16) & 255) / 255, double((n >> 8) & 255) / 255, double(n & 255) / 255, 1, {}};
}

/// The edits made at once, as one undo step (or, during a Magic edit, in its draft).
/// Who's calling, for the cursor that plays their edits.
thread_local QString performer;
thread_local Controller* performing = nullptr;

void perform(const std::vector<MagicEdit>& edits, EditorModel& m, MCPDraft* draft) {
    if (draft) return draft->apply(edits);
    // Played with the agent's own cursor (MagicShow.perform), and waited for; or made at once.
    if (performing && !performer.isEmpty() && MagicShow::animated() && !MagicShow::running()) {
        MagicPlan plan{m.project(), {}};
        for (size_t i = 0; i < edits.size(); ++i) plan.tasks.push_back({edits[i], i > 0 ? std::vector<int>{int(i) - 1} : std::vector<int>{}, 0});
        bool over = false;
        QEventLoop loop;
        MagicShow::play(*performing, plan, [&](bool) { over = true; loop.quit(); }, MagicShow::Performer{performer, MagicShow::color(performer)});
        if (!over) loop.exec();
        return;
    }
    m.mutate([&](Project& p) { for (const auto& e : edits) e.apply(p); });
}

MagicEdit addTrackEdit(TrackKind kind, const Uuid& id) {
    return MagicEdit::change(std::nullopt, kind == TrackKind::video ? QStringLiteral("Add video track") : QStringLiteral("Add audio track"), [kind, id](Project& q) {
        Track t(kind);
        t.id = id;
        if (kind == TrackKind::video) q.videoTracks.push_back(t); else q.audioTracks.push_back(t);
    });
}

QByteArray jpeg(const QImage& img) {
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    img.save(&b, "JPEG", 80);
    return out;
}

/// ContactSheet: a photo as it is, or frames from across a video in a grid, each marked with its time.
std::optional<std::pair<QByteArray, std::vector<double>>> contactSheet(Controller& c, const MediaAsset& asset, int frames, std::vector<double> around) {
    if (asset.kind == MediaKind::image) {
        auto img = Stills::load(c.media().readPath(asset), 768);
        if (!img) return std::nullopt;
        return std::make_pair(jpeg(*img), std::vector<double>{});
    }
    if (asset.kind != MediaKind::video || !(asset.duration > 0)) return std::nullopt;
    std::vector<double> wanted;
    if (!around.empty()) {
        for (double t : around) if (wanted.size() < 12) wanted.push_back(std::min(std::max(0.0, t), std::max(0.0, asset.duration - 0.05)));
    } else {
        const int n = std::max(1, std::min(12, frames));
        for (int i = 0; i < n; ++i) wanted.push_back((i + 0.5) * asset.duration / n);
    }
    std::sort(wanted.begin(), wanted.end());
    auto dec = VideoDecoder::open(c.media().readPath(asset));
    if (!dec) return std::nullopt;
    const MediaRuntime* rt = c.media().runtime(asset.id);
    std::vector<std::pair<double, QImage>> got;
    for (double t : wanted) {
        dec->seek(t);
        if (auto f = dec->frameAt(t)) {
            f->orientation = rt ? rt->orientation : 1;
            got.emplace_back(t, Thumbnails::fit(*f, rt ? rt->color : SourceColor::standard, 360));
        }
    }
    if (got.empty()) return std::nullopt;
    // A grid of up to four across, each cell the frame's shape, with its time in the corner.
    const int count = int(got.size());
    const int columns = std::min(4, count <= 4 ? count : count <= 6 ? 3 : 4);
    const int rows = (count + columns - 1) / columns;
    const int cellW = got.front().second.width(), cellH = got.front().second.height(), gap = 4;
    QImage sheet(columns * cellW + (columns - 1) * gap, rows * cellH + (rows - 1) * gap, QImage::Format_RGB32);
    sheet.fill(QColor::fromRgbF(0.1f, 0.1f, 0.1f));
    QPainter p(&sheet);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(15);
    font.setBold(true);
    p.setFont(font);
    std::vector<double> times;
    for (int i = 0; i < count; ++i) {
        const QRect cell((i % columns) * (cellW + gap), (i / columns) * (cellH + gap), cellW, cellH);
        const QImage& img = got[size_t(i)].second;
        const QSize s = img.size().scaled(cell.size(), Qt::KeepAspectRatio);
        p.drawImage(QRect(cell.center().x() - s.width() / 2, cell.center().y() - s.height() / 2, s.width(), s.height()), img);
        const QString label = QString::asprintf("%.1fs", got[size_t(i)].first);
        const QRect box(cell.left() + 4, cell.top() + 4, p.fontMetrics().horizontalAdvance(label) + 8, p.fontMetrics().height() + 4);
        p.fillRect(box, QColor(0, 0, 0, 153));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
        times.push_back(got[size_t(i)].first);
    }
    p.end();
    return std::make_pair(jpeg(sheet), times);
}

/// EditorModel.snapshot: the cut at a time, no bigger than 640 on its long side.
QByteArray snapshotPNG(Controller& c, const Project& p, double t) {
    const double scale = std::min(1.0, 640.0 / std::max(p.width, p.height));
    BuildOptions o;
    o.renderW = int(std::max(2.0, std::round(p.width * scale / 2) * 2));
    o.renderH = int(std::max(2.0, std::round(p.height * scale / 2) * 2));
    auto plan = CompositionBuilder::build(p, c.media(), o);
    if (!plan) return {};
    t = std::min(std::max(0.0, t), std::max(0.0, plan->duration - 1 / p.fps));
    const auto px = CpuRenderer::render(describeFrame(*plan, t, c.media().provider(std::max(o.renderW, o.renderH))));
    QImage img(px.data(), o.renderW, o.renderH, QImage::Format_RGBX8888);
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    img.save(&b, "PNG");
    return out;
}

/// Imports files now (MCP's import waits for them), with no "match the sequence" question.
std::vector<MediaAsset> importNow(Controller& c, const QStringList& paths) {
    QStringList files;
    for (const QString& p : paths) {
        if (MediaFolders::isFolder(p)) files += MediaFolders::mediaFiles(p);
        else files << p;
    }
    std::vector<MediaAsset> out;
    for (const QString& f : files) {
        const Project& p = c.model().project();
        const auto existing = std::find_if(p.assets.begin(), p.assets.end(), [&](const MediaAsset& a) { return a.path == f; });
        if (existing != p.assets.end()) { out.push_back(*existing); continue; }
        auto future = QtConcurrent::run([&c, f] { return c.media().importFile(QFileInfo(f).absoluteFilePath()); });
        while (!future.isFinished()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (auto a = future.result()) {
            c.model().changeLibrary([&](Project& q) { q.assets.push_back(*a); });
            out.push_back(*a);
        }
    }
    return out;
}

/// MediaDownload.fetch: one audio file from an https link into "Downloaded Music", with its source and
/// licence written beside it.
QString download(Controller& c, const QUrl& url, const QString& name, const QString& sourcePage, const QString& licence) {
    struct Failure { QString message; };
    if (url.scheme() != QLatin1String("https")) throw MCPToolError{QStringLiteral("Use an https link to the audio file.")};
    QNetworkAccessManager net;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Editroom"));
    req.setTransferTimeout(120000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = net.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0)
        throw MCPToolError{QStringLiteral("Couldn't download it: %1").arg(reply->errorString())};
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status && (status < 200 || status >= 300)) throw MCPToolError{QStringLiteral("The site answered %1; that link didn't give a file.").arg(status)};
    const QString mime = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(u';', 0, 0).trimmed();
    if (mime.startsWith(QLatin1String("text/")) || mime.contains(QLatin1String("html")) || mime.contains(QLatin1String("json")))
        throw MCPToolError{QStringLiteral("That link is a web page, not an audio file. Find the direct link to the file (it usually ends in .mp3 or .wav).")};
    const QByteArray data = reply->readAll();
    if (data.isEmpty()) throw MCPToolError{QStringLiteral("The download was empty.")};
    if (data.size() > 200LL * 1024 * 1024) throw MCPToolError{QStringLiteral("That file is over 200 MB, too big for a music track.")};
    static const QStringList audioTypes = {"mp3", "m4a", "aac", "wav", "aif", "aiff", "caf", "flac"};
    QString ext = QFileInfo(url.path()).suffix().toLower();
    if (!audioTypes.contains(ext)) {
        static const std::map<QString, QString> byMime = {{"audio/mpeg", "mp3"}, {"audio/mp3", "mp3"}, {"audio/mp4", "m4a"}, {"audio/x-m4a", "m4a"},
            {"audio/aac", "m4a"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"}, {"audio/wave", "wav"}, {"audio/aiff", "aiff"},
            {"audio/x-aiff", "aiff"}, {"audio/flac", "flac"}, {"audio/x-flac", "flac"}};
        auto it = byMime.find(mime);
        ext = it != byMime.end() ? it->second : QStringLiteral("mp3");
    }
    const QString project = c.model().fileName();
    const QString folder = !project.isEmpty() ? QFileInfo(project).absolutePath() + QStringLiteral("/Downloaded Music")
                                              : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Downloaded Music");
    QDir().mkpath(folder);
    QString base = name.isEmpty() ? QFileInfo(url.path()).completeBaseName() : name;
    static const QRegularExpression bad(QStringLiteral("[/\\\\:?%*|\"<>\\n\\r\\x00-\\x1f]"));
    base = base.replace(bad, QStringLiteral(" ")).trimmed().left(80);
    if (base.isEmpty()) base = QStringLiteral("Music");
    QString dest = folder + u'/' + base + u'.' + ext;
    for (int n = 2; QFileInfo::exists(dest); ++n) dest = folder + u'/' + QStringLiteral("%1 %2.").arg(base).arg(n) + ext;
    QFile f(dest);
    if (!f.open(QIODevice::WriteOnly)) throw MCPToolError{QStringLiteral("Couldn't save it.")};
    f.write(data);
    f.close();
    // Only keep it if it really is sound, at least a second long.
    auto probe = Probe::file(dest);
    if (!probe || !probe->hasAudio || probe->duration < 1) {
        QFile::remove(dest);
        throw MCPToolError{QStringLiteral("That file isn't audio EditroomLX can play. Find the direct link to an .mp3 or .wav.")};
    }
    QFile note(folder + QStringLiteral("/Sources.txt"));
    if (note.open(QIODevice::Append)) {
        note.write(QStringLiteral("%1\n  Downloaded: %2\n  File: %3\n  Found on: %4\n  Licence: %5")
                       .arg(QFileInfo(dest).fileName(), QDateTime::currentDateTimeUtc().toString(Qt::ISODate), url.toString(),
                            sourcePage.isEmpty() ? QStringLiteral("not given") : sourcePage, licence.isEmpty() ? QStringLiteral("not given") : licence)
                       .toUtf8());
    }
    return dest;
}

} // namespace

namespace {
/// Transcripts.ready: the transcript, made now if it isn't yet, waiting up to `limit` seconds (the app goes on
/// meanwhile).
std::optional<Transcript> waitTranscript(Controller& ctl, const QString& path, double limit = 90) {
    std::optional<Transcript> got;
    bool done = false;
    QEventLoop loop;
    ctl.transcripts().ready(path, [&](std::optional<Transcript> t) { got = t; done = true; loop.quit(); }, limit);
    if (!done) loop.exec();
    return got;
}

QJsonObject transcriptJSON(const Transcript& t, const MediaAsset& a, bool words) {
    QJsonObject d{{"asset_id", uuid(a.id)}, {"name", a.name}, {"duration", r3(a.duration)}};
    if (t.isEmpty()) {
        d.insert("status", "no_speech");
        d.insert("note", "Nobody talks in it (or only music or noise).");
        return d;
    }
    d.insert("status", "ready");
    d.insert("language", t.language);
    d.insert("times", "Seconds in the file. A clip plays the file from its source_start.");
    QJsonArray sentences;
    for (const auto& s : t.sentences()) sentences.append(QJsonObject{{"start", r3(s.start)}, {"end", r3(s.end)}, {"text", s.text}});
    d.insert("sentences", sentences);
    // Silence of half a second or more, which is where cuts can go (filler like "um" isn't written out, so it shows
    // as a pause).
    QJsonArray pauses;
    double at = 0;
    for (const auto& s : t.speech()) {
        if (s.start - at >= 0.5) pauses.append(QJsonObject{{"start", r3(at)}, {"end", r3(s.start)}});
        at = s.end;
    }
    if (a.duration - at >= 0.5) pauses.append(QJsonObject{{"start", r3(at)}, {"end", r3(a.duration)}});
    d.insert("pauses", pauses);
    if (words) {
        QJsonArray w;
        for (const auto& x : t.words) w.append(QJsonArray{r3(x.start), r3(x.end), x.text});
        d.insert("words", w);
    }
    return d;
}

QJsonValue captionsJSON(const Project& p, Controller& ctl) {
    if (!p.captions) return QJsonValue::Null;
    const auto blocks = CaptionLayout::blocks(p, [&ctl](const MediaAsset& a) { return ctl.media().transcript(a.id); });
    QJsonArray ids, caps;
    for (const auto& c : p.captionedClips()) ids.append(uuid(c.id));
    for (size_t i = 0; i < blocks.size() && i < 60; ++i) caps.append(QJsonObject{{"start", r3(blocks[i].start)}, {"end", r3(blocks[i].end)}, {"text", blocks[i].text()}});
    return QJsonObject{{"hidden", p.captions->hidden}, {"size", p.captions->style.size}, {"position", p.captions->style.y},
                       {"highlight", p.captions->style.highlight}, {"clip_ids", ids}, {"count", int(blocks.size())}, {"captions", caps}};
}
} // namespace

QJsonArray MCPTools::call(const QString& name, const QJsonObject& args, Controller& ctl, const QString& who, MCPDraft* draft) {
    performer = who;
    performing = &ctl;

    EditorModel& m = ctl.model();
    if (draft) ++draft->calls;
    /// The project as it is now: the Magic draft's while there is one.
    const Project p = draft ? draft->project : m.project();
    auto now = [&m, draft]() -> const Project& { return draft ? draft->project : m.project(); };
    const std::string n = name.toStdString();
    auto notInMagic = [&] { if (draft) throw MCPToolError{QStringLiteral("%1 isn't available while making a Magic edit.").arg(name)}; };
    const double playhead = draft ? draft->playhead : m.playhead();

    if (n == "get_project") return jsonOut(projectJSON(p, m.title(), playhead));
    if (n == "list_media") {
        // Labels most of the library shares ("structure", "people" in a vlog) don't tell clips apart: left out.
        std::vector<SceneTags> tagged;
        for (const auto& a : p.assets) if (auto t = ctl.sceneTags(a.id)) tagged.push_back(*t);
        std::map<QString, int> counts;
        for (const auto& t : tagged) {
            std::set<QString> names;
            for (const auto& g : t.tags) names.insert(g.name);
            for (const auto& nm : names) ++counts[nm];
        }
        std::set<QString> common;
        if (tagged.size() >= 4) for (const auto& [nm, k] : counts) if (double(k) >= double(tagged.size()) * 0.6) common.insert(nm);
        const MediaTree tree(p);
        QJsonArray out;
        for (const auto& a : p.assets) {
            QJsonObject d = assetJSON(a, ctl, common);
            if (auto at = tree.folderOf(a)) d.insert("folder", tree.label(*at));
            out.append(d);
        }
        return jsonOut(out);
    }
    if (n == "list_looks") {
        QJsonArray a;
        for (const auto& [look, r] : BuiltInLook::all()) a.append(look);
        return jsonOut(a);
    }
    if (n == "import_media") {
        notInMagic();
        QStringList paths;
        QStringList missing;
        for (const auto& v : args.value("paths").toArray()) {
            QString s = v.toString();
            if (s.startsWith(QLatin1String("~/"))) s = QDir::homePath() + s.mid(1);
            paths << s;
            if (!QFileInfo::exists(s)) missing << s;
        }
        if (!missing.isEmpty()) throw MCPToolError{QStringLiteral("No file at ") + missing.join(QStringLiteral(", "))};
        const auto assets = importNow(ctl, paths);
        QJsonArray imported;
        for (const auto& a : assets) imported.append(assetJSON(a));
        return jsonOut(QJsonObject{{"imported", imported}, {"failed", int(paths.size()) - int(assets.size())}});
    }
    if (n == "download_media") {
        if (!args.value("url").isString()) throw MCPToolError{QStringLiteral("url is needed: the https link to the audio file.")};
        const QString file = download(ctl, QUrl(args.value("url").toString().trimmed()), args.value("name").toString(),
                                      args.value("source_page").toString(), args.value("licence").toString());
        const auto assets = importNow(ctl, {file});
        if (assets.empty()) throw MCPToolError{QStringLiteral("Downloaded it, but couldn't add it to the library.")};
        if (draft) draft->project.assets.push_back(assets.front());
        QJsonObject out = assetJSON(assets.front());
        out.insert("saved_to", file);
        return jsonOut(QJsonObject{{"added", out}, {"note", "In the library now; add it with add_clip."}});
    }
    if (n == "analyze_media") {
        const MediaAsset& asset = assetArg(args, p);
        if (asset.kind == MediaKind::audio) {
            auto mu = MediaAnalyzer::music(ctl.media().readPath(asset));
            if (!mu) throw MCPToolError{QStringLiteral("Couldn't read the sound in %1.").arg(asset.name)};
            QJsonArray bars;
            for (int i = 0; i < int(mu->beats.size()) && bars.size() < 80; ++i) if (mu->isBarStart(i)) bars.append(r3(mu->beats[size_t(i)]));
            return jsonOut(QJsonObject{{"bpm", r3(mu->bpm)}, {"beat_length", r3(mu->period())}, {"beat_count", int(mu->beats.size())},
                                       {"first_beat", r3(mu->beats.empty() ? 0 : mu->beats.front())}, {"bar_starts", bars}});
        }
        const MediaRuntime* rt = ctl.media().runtime(asset.id);
        auto a = MediaAnalyzer::clip(ctl.media().readPath(asset), asset.kind == MediaKind::image, rt ? rt->orientation : 1);
        if (!a) throw MCPToolError{QStringLiteral("Couldn't look at %1.").arg(asset.name)};
        const double length = number(args, "moment_length").value_or(2);
        const auto score = AutoPlanner::scorer(a->samples);
        QJsonArray best;
        for (const auto& mo : AutoPlanner::moments(*a, asset.kind == MediaKind::image ? 5 : asset.duration, length, 5, score))
            best.append(QJsonObject{{"start", r3(mo.start)}, {"end", r3(mo.end)}, {"score", r3(mo.score)}, {"faces", mo.faces}});
        double mean = 0;
        bool faces = false, utility = false;
        for (const auto& s : a->samples) { mean += s.luma; faces = faces || s.faces > 0; utility = utility || s.utility == true; }
        mean /= std::max<size_t>(1, a->samples.size());
        QJsonObject out{{"duration", r3(std::min(a->duration, asset.kind == MediaKind::image ? 5.0 : asset.duration))}, {"best_moments", best},
                        {"average_brightness", r3(mean)}, {"has_faces", faces}};
        if (a->created) out.insert("filmed", QDateTime::fromSecsSinceEpoch(qint64(*a->created + 978307200.0), QTimeZone::UTC).toString(Qt::ISODate));
        if (utility) out.insert("looks_like_a_screenshot_or_document", true);
        return jsonOut(out);
    }
    if (n == "get_transcript") {
        const MediaAsset& a = assetArg(args, p);
        const QString path = ctl.media().soundPath(a.id);
        if (a.kind == MediaKind::image || path.isEmpty())
            return jsonOut(QJsonObject{{"asset_id", uuid(a.id)}, {"status", "no_sound"}, {"note", QStringLiteral("%1 has no sound.").arg(a.name)}});
        if (!Transcripts::available())
            return jsonOut(QJsonObject{{"asset_id", uuid(a.id)}, {"status", "unavailable"},
                                       {"note", "Hearing speech needs EditroomLX's speech model, which the person can download in Settings › General › Speech."}});
        auto t = waitTranscript(ctl, path);
        if (!t) return jsonOut(QJsonObject{{"asset_id", uuid(a.id)}, {"status", "failed"}, {"note", QStringLiteral("Couldn't hear what's said in %1.").arg(a.name)}});
        return jsonOut(transcriptJSON(*t, a, args.value("words").toBool(false)));
    }
    if (n == "look_at_media") {
        const MediaAsset& a = assetArg(args, p);
        if (a.kind == MediaKind::audio) throw MCPToolError{QStringLiteral("%1 is sound only; analyze_media finds its beats.").arg(a.name)};
        std::vector<double> times;
        for (const auto& v : args.value("times").toArray()) if (v.isDouble()) times.push_back(v.toDouble());
        auto sheet = contactSheet(ctl, a, args.value("frames").toInt(6), times);
        if (!sheet) throw MCPToolError{QStringLiteral("Couldn't look at %1.").arg(a.name)};
        QString note;
        if (a.kind == MediaKind::image) {
            note = QStringLiteral("%1, a photo.").arg(a.name);
        } else {
            QStringList ts;
            for (double t : sheet->second) ts << QString::asprintf("%.1fs", t);
            note = QStringLiteral("%1, %2s long. Frames at %3, left to right, top to bottom.").arg(a.name, json::formatNumber(r3(a.duration)), ts.join(QStringLiteral(", ")));
        }
        const auto saved = MediaNotes::note(a.path);
        note += saved ? QStringLiteral(" Its saved notes: %1 Update them with note_media if you see more.").arg(saved->text)
                      : QStringLiteral(" Save what you see with note_media.");
        return QJsonArray{QJsonObject{{"type", "image"}, {"data", QString::fromLatin1(sheet->first.toBase64())}, {"mimeType", "image/jpeg"}},
                          QJsonObject{{"type", "text"}, {"text", note}}};
    }
    if (n == "note_media") {
        const MediaAsset& a = assetArg(args, p);
        const QString t = args.value("notes").toString().trimmed();
        if (t.isEmpty()) throw MCPToolError{QStringLiteral("Give notes: what's in it, with times.")};
        MediaNotes::save(t, a.path, who);
        return text(QStringLiteral("Saved. list_media will show it with %1.").arg(a.name));
    }
    if (n == "get_frame") {
        if (p.clips.empty()) throw MCPToolError{QStringLiteral("The timeline is empty.")};
        const double t = number(args, "time").value_or(playhead);
        const QByteArray png = snapshotPNG(ctl, p, t);
        if (png.isEmpty()) throw MCPToolError{QStringLiteral("Couldn't render a frame at %1s.").arg(t)};
        return QJsonArray{QJsonObject{{"type", "image"}, {"data", QString::fromLatin1(png.toBase64())}, {"mimeType", "image/png"}},
                          QJsonObject{{"type", "text"}, {"text", QStringLiteral("Frame at %1s.").arg(json::formatNumber(r3(t)))}}};
    }
    if (n == "add_clip") {
        const MediaAsset& asset = assetArg(args, p);
        const bool audioOnly = asset.kind == MediaKind::audio;
        const Uuid track = trackArg(args.value("track").toString(audioOnly ? QStringLiteral("A1") : QStringLiteral("V1")), p, audioOnly ? TrackKind::audio : TrackKind::video);
        double lastEnd = 0;
        for (const auto& c : p.clips) if (c.trackID == track) lastEnd = std::max(lastEnd, c.end());
        const double start = std::max(0.0, number(args, "start").value_or(lastEnd));
        const double source = std::max(0.0, number(args, "source_start").value_or(0));
        if (!(asset.kind == MediaKind::image || source < asset.duration - 0.05))
            throw MCPToolError{QStringLiteral("source_start is past the end of %1 (%2s).").arg(asset.name, json::formatNumber(r3(asset.duration)))};
        const double rest = asset.kind == MediaKind::image ? 5 : asset.duration - source;
        const double length = std::max(1 / p.fps, std::min(number(args, "duration").value_or(rest), asset.kind == MediaKind::image ? 3600 : rest));
        const Uuid audioTrack = audioOnly ? track : p.freeTrack(TrackKind::audio, start, length).value_or(p.audioTracks.empty() ? TrackToken::newAudio() : p.audioTracks.front().id);
        auto clips = m.makeClips(DragPayload::ofAsset(asset.id), p.snapFrame(start), audioOnly ? (p.videoTracks.empty() ? track : p.videoTracks.front().id) : track, audioTrack);
        for (auto& c : clips) { c.sourceStart = asset.kind == MediaKind::image ? 0 : source; c.duration = p.snapFrame(length); }
        if (clips.empty()) throw MCPToolError{QStringLiteral("Couldn't make a clip from %1.").arg(asset.name)};
        perform({MagicEdit::place(clips, asset.id)}, m, draft);
        QJsonArray added;
        for (const auto& c : clips) added.append(QJsonObject{{"id", uuid(c.id)}, {"track", trackName(c.trackID, now())}, {"start", r3(c.start)}, {"end", r3(c.end())}});
        return jsonOut(QJsonObject{{"added", added}});
    }
    if (n == "add_title") {
        const QString words = args.value("text").toString().trimmed();
        if (words.isEmpty()) throw MCPToolError{QStringLiteral("The title needs some text.")};
        const double start = std::max(0.0, number(args, "start").value_or(0));
        const double length = std::max(0.2, number(args, "duration").value_or(3));
        Uuid track;
        std::vector<MagicEdit> edits;
        if (args.contains("track")) {
            track = trackArg(args.value("track").toString(), p, TrackKind::video);
        } else {
            // Above the picture: the first video track (not V1) that's free there.
            std::optional<Uuid> free;
            for (size_t i = 1; i < p.videoTracks.size() && !free; ++i) {
                const Uuid id = p.videoTracks[i].id;
                if (std::none_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == id && c.start < start + length && c.end() > start; })) free = id;
            }
            if (free) { track = *free; }
            else { track = newUuid(); edits.push_back(addTrackEdit(TrackKind::video, track)); }
        }
        Clip clip(track, ClipKind::text, p.snapFrame(start), p.snapFrame(length));
        clip.text.string = words;
        edits.push_back(MagicEdit::place({clip}, std::nullopt));
        perform(edits, m, draft);
        return jsonOut(QJsonObject{{"added", QJsonObject{{"id", uuid(clip.id)}, {"track", trackName(track, now())}, {"start", r3(clip.start)}, {"end", r3(clip.end())}}}});
    }
    if (n == "move_clip") {
        const Clip& c = clipArg(args, p);
        const double start = std::max(0.0, numArg(args, "start"));
        std::map<Uuid, Uuid> tracks;
        if (args.contains("track")) tracks[c.id] = trackArg(args.value("track").toString(), p, trackKindOf(c.kind));
        const IdSet group = p.unlocked(p.linked({c.id}));
        MagicEdit e;
        e.kind = MagicEdit::Kind::move;
        e.ids.assign(group.begin(), group.end());
        e.from = c.start;
        e.to = p.snapFrame(start);
        e.tracks = tracks;
        perform({e}, m, draft);
        return jsonOut(clipsJSON(e.ids, now()));
    }
    if (n == "trim_clip") {
        const Clip& c = clipArg(args, p);
        const IdSet group = p.unlocked(p.linked({c.id}));
        const std::vector<Uuid> ids(group.begin(), group.end());
        double newStart = c.start, newEnd = c.end();
        if (auto s = number(args, "start")) {
            // Not before the start of the file.
            const double earliest = c.playsSource() ? c.start - c.timelineSpan(0, c.sourceStart) : 0;
            newStart = p.snapFrame(std::max(std::max(0.0, earliest), *s));
        }
        if (auto e = number(args, "end")) {
            double latest = INFINITY;
            if (c.playsSource()) if (auto limit = p.sourceLimit(c)) latest = c.start + c.timelineSpan(c.sourceStart, *limit);
            newEnd = p.snapFrame(std::min(latest, *e));
        }
        if (!(newEnd - newStart >= 1 / p.fps)) throw MCPToolError{QStringLiteral("That would leave the clip with no length.")};
        std::vector<MagicEdit> edits;
        if (newStart != c.start) { MagicEdit e; e.kind = MagicEdit::Kind::trimHead; e.ids = ids; e.from = c.start; e.to = newStart; edits.push_back(e); }
        if (newEnd != c.end()) { MagicEdit e; e.kind = MagicEdit::Kind::trimTail; e.ids = ids; e.from = c.end(); e.to = newEnd; edits.push_back(e); }
        if (!edits.empty()) perform(edits, m, draft);
        return jsonOut(clipsJSON(ids, now()));
    }
    if (n == "set_speed") {
        const Clip& c = clipArg(args, p);
        if (!c.playsSource()) throw MCPToolError{QStringLiteral("Only video and sound clips have a speed.")};
        const double speed = std::clamp(numArg(args, "speed"), 0.05, 100.0);
        const IdSet ids = p.unlocked(p.linked({c.id}));
        perform({MagicEdit::change(c.id, QString(), [ids, speed](Project& q) { q.setSpeed(ids, speed); })}, m, draft);
        return jsonOut(clipsJSON({ids.begin(), ids.end()}, now()));
    }
    if (n == "set_speed_ramp") {
        const Clip& c = clipArg(args, p);
        if (!c.playsSource()) throw MCPToolError{QStringLiteral("Only video and sound clips have a speed.")};
        const IdSet ids = p.unlocked(p.linked({c.id}));
        if (args.value("remove").toBool()) {
            perform({MagicEdit::change(c.id, QStringLiteral("No ramp"), [ids](Project& q) { q.retime(ids, false, [](Clip& x) { x.ramp.reset(); }); })}, m, draft);
            return jsonOut(clipsJSON({ids.begin(), ids.end()}, now()));
        }
        auto speedV = number(args, "speed");
        if (!speedV) throw MCPToolError{QStringLiteral("speed is needed (or remove: true).")};
        const double speed = std::clamp(*speedV, 0.05, 100.0);
        const double s0 = c.sourceStart, L = std::max(1e-6, c.sourceLength()), s1 = s0 + L;
        const double ease = std::max(0.04, number(args, "ease").value_or(0.4));
        const double around = std::clamp(number(args, "around").value_or(1), 0.05, 100.0);
        std::optional<double> from = number(args, "from");
        if (from) from = std::min(std::max(*from, s0), s1);
        std::optional<double> to = number(args, "to");
        if (to) to = std::min(std::max(*to, from.value_or(s0)), s1);
        if (!from && !to) throw MCPToolError{QStringLiteral("Give from and/or to (seconds in the file); for one speed throughout, use set_speed.")};
        auto f = [&](double s) { return std::min(1.0, std::max(0.0, (s - s0) / L)); };
        double p1 = 0, p2 = 0, p3 = 1, p4 = 1;
        if (from && *from > s0 + 1e-3) { p1 = f(*from - ease); p2 = f(*from); }
        if (to && *to < s1 - 1e-3) { p3 = f(*to); p4 = f(*to + ease); }
        if (p2 - p1 < 0.001 && p4 - p3 < 0.001)
            throw MCPToolError{QStringLiteral("That ramp would sit outside the clip: from and to should be inside its part of the file (%1–%2s).").arg(QString::asprintf("%.2f", s0), QString::asprintf("%.2f", s1))};
        perform({MagicEdit::change(c.id, speed < around ? QStringLiteral("Slow-mo ramp") : QStringLiteral("Speed ramp"),
                                   [=](Project& q) { q.setRamp(ids, speed, p1, p2, p3, p4, around, around); })}, m, draft);
        return jsonOut(clipsJSON({ids.begin(), ids.end()}, now()));
    }
    if (n == "set_captions") {
        IdSet given;
        const QJsonArray raw = args.value("clip_ids").toArray();
        for (const auto& v : raw) { const Uuid id(v.toString()); if (!id.isNull() && p.clip(id)) given.insert(id); }
        if (!raw.isEmpty() && given.empty()) throw MCPToolError{QStringLiteral("clip_ids aren't clip ids from get_project.")};
        if (args.value("remove").toBool(false)) {
            const IdSet targets = p.captionTargets(given);
            const bool all = given.empty();
            perform({MagicEdit::change(targets.empty() ? std::nullopt : std::optional<Uuid>(*targets.begin()), all ? QStringLiteral("No captions") : QStringLiteral("Remove captions"),
                                       [=](Project& q) { if (all) q.removeCaptionTrack(); else q.removeCaptions(targets); })}, m, draft);
            return jsonOut(QJsonObject{{"captions", captionsJSON(draft ? draft->project : m.project(), ctl)}});
        }
        if (!Transcripts::available())
            throw MCPToolError{QStringLiteral("Captions need EditroomLX's speech model, which the person can download in Settings › General › Speech.")};
        const IdSet targets = p.unlocked(given.empty() ? (p.captions ? IdSet() : p.defaultCaptionTargets()) : p.captionTargets(given));
        if (!given.empty() && targets.empty()) throw MCPToolError{QStringLiteral("Those clips have no sound to caption.")};
        if (!p.captions && targets.empty()) throw MCPToolError{QStringLiteral("There's no one talking to caption: give clip_ids of clips with speech.")};
        if (args.contains("edit") && !p.captions && targets.empty()) throw MCPToolError{QStringLiteral("There are no captions to edit.")};
        // What's said in them, heard first, so the captions are there when this returns.
        for (const auto& c : p.clips)
            if (targets.count(c.id) && c.assetID) if (const QString sp = ctl.media().soundPath(*c.assetID); !sp.isEmpty()) waitTranscript(ctl, sp);
        // Captions to fix, found on the captions as they'll be once these clips are captioned.
        Project after = p;
        after.addCaptions(targets);
        const auto shown = CaptionLayout::blocks(after, [&ctl](const MediaAsset& a) { return ctl.media().transcript(a.id); });
        std::vector<std::pair<CaptionBlock, QString>> fixes;
        for (const auto& ev : args.value("edit").toArray()) {
            const QJsonObject e = ev.toObject();
            if (!e.value("at").isDouble() || !e.value("text").isString()) throw MCPToolError{QStringLiteral("Each edit needs at (timeline seconds) and text.")};
            const double at = e.value("at").toDouble();
            std::optional<CaptionBlock> near;
            for (const auto& b : shown) if (at >= b.start && at < b.end) { near = b; break; }
            if (!near && !shown.empty()) {
                const CaptionBlock* best = &shown.front();
                for (const auto& b : shown) if (std::abs((b.start + b.end) / 2 - at) < std::abs((best->start + best->end) / 2 - at)) best = &b;
                if (at >= best->start - 0.5 && at < best->end + 0.5) near = *best;
            }
            if (!near) throw MCPToolError{QStringLiteral("No caption at %1s.").arg(QString::asprintf("%.2f", at))};
            fixes.push_back({*near, e.value("text").toString()});
        }
        const auto size = number(args, "size");
        const auto position = number(args, "position");
        const std::optional<bool> highlight = args.value("highlight").isBool() ? std::optional<bool>(args.value("highlight").toBool()) : std::nullopt;
        const std::optional<bool> hidden = args.value("hidden").isBool() ? std::optional<bool>(args.value("hidden").toBool()) : std::nullopt;
        perform({MagicEdit::change(targets.empty() ? std::nullopt : std::optional<Uuid>(*targets.begin()), QStringLiteral("Captions"), [=](Project& q) {
            q.addCaptions(targets);
            if (!q.captions) return;
            if (size) q.captions->style.size = std::clamp(*size, CaptionStyle::sizeMin, CaptionStyle::sizeMax);
            if (position) q.captions->style.y = std::clamp(*position, 0.1, 0.95);
            if (highlight) q.captions->style.highlight = *highlight;
            if (hidden) q.captions->hidden = *hidden;
            for (const auto& [b, text] : fixes) q.setCaptionText(b, text);
        })}, m, draft);
        return jsonOut(QJsonObject{{"captions", captionsJSON(draft ? draft->project : m.project(), ctl)}});
    }
    if (n == "split_clip") {
        const Clip& c = clipArg(args, p);
        const double at = p.snapFrame(numArg(args, "at"));
        if (!(at > c.start + 0.5 / p.fps && at < c.end() - 0.5 / p.fps))
            throw MCPToolError{QStringLiteral("%1s isn't inside the clip (%2–%3).").arg(json::formatNumber(r3(at)), json::formatNumber(r3(c.start)), json::formatNumber(r3(c.end())))};
        const IdSet group = p.unlocked(p.linked({c.id}));
        MagicEdit e;
        e.kind = MagicEdit::Kind::blade;
        e.ids.assign(group.begin(), group.end());
        e.at = at;
        for (const auto& id : e.ids) e.pieces[id] = newUuid();
        e.link = newUuid();
        perform({e}, m, draft);
        std::vector<Uuid> right;
        for (const auto& id : e.ids) right.push_back(e.pieces[id]);
        return jsonOut(QJsonObject{{"left", clipsJSON(e.ids, now())}, {"right", clipsJSON(right, now())}});
    }
    if (n == "delete_clips") {
        IdSet ids;
        for (const auto& v : args.value("clip_ids").toArray()) {
            auto id = json::parseUuid(v.toString());
            if (!id || !p.clip(*id)) throw MCPToolError{QStringLiteral("No clip with id %1.").arg(v.toString())};
            ids.insert(*id);
        }
        const IdSet all = p.unlocked(p.linked(ids));
        if (all.empty()) throw MCPToolError{QStringLiteral("Nothing to delete (locked tracks?).")};
        MagicEdit e;
        e.kind = MagicEdit::Kind::remove;
        e.ids.assign(all.begin(), all.end());
        e.ripple = args.value("ripple").toBool();
        perform({e}, m, draft);
        return text(QStringLiteral("Deleted %1 clip%2.").arg(all.size()).arg(all.size() == 1 ? QString() : QStringLiteral("s")));
    }
    if (n == "set_grade") {
        QStringList raw;
        if (args.value("clip_ids").isArray()) for (const auto& v : args.value("clip_ids").toArray()) raw << v.toString();
        else if (args.value("clip_id").isString()) raw << args.value("clip_id").toString();
        std::vector<MagicEdit> edits;
        std::vector<Uuid> done;
        for (const QString& s : raw) {
            auto id = json::parseUuid(s);
            const Clip* c = id ? p.clip(*id) : nullptr;
            if (!c) throw MCPToolError{QStringLiteral("No clip with id %1.").arg(s)};
            if (c->kind == ClipKind::audio) throw MCPToolError{QStringLiteral("%1 is sound; it has no colour.").arg(p.name(*c))};
            done.push_back(*id);
            const Uuid cid = *id;
            if (args.value("look").isString()) {
                const QString look = args.value("look").toString();
                if (look.toLower() == QLatin1String("none")) {
                    edits.push_back(MagicEdit::change(cid, QStringLiteral("No look"), [cid](Project& q) {
                        if (auto i = q.index(cid)) { ColorGrade g = q.clips[*i].grade.value_or(ColorGrade()); g.look.reset(); if (g.isNeutral()) q.clips[*i].grade.reset(); else q.clips[*i].grade = g; }
                    }));
                } else {
                    QString path = look.startsWith(QLatin1String("~/")) ? QDir::homePath() + look.mid(1) : look;
                    const bool builtIn = BuiltInLook::named(look).has_value();
                    if (!builtIn && !(path.endsWith(QLatin1String(".cube")) && GradeBaker::cubeFile(path)))
                        throw MCPToolError{QStringLiteral("No look called %1. list_looks has the built-in ones.").arg(look)};
                    MagicEdit e;
                    e.kind = MagicEdit::Kind::look;
                    e.clip = cid;
                    e.text = builtIn ? look : path;
                    edits.push_back(e);
                }
            }
            const std::pair<const char*, MagicGradeKey> keys[] = {{"exposure", MagicGradeKey::exposure}, {"contrast", MagicGradeKey::contrast}, {"saturation", MagicGradeKey::saturation}};
            for (const auto& [field, key] : keys) {
                if (auto v = number(args, field)) {
                    MagicEdit e;
                    e.kind = MagicEdit::Kind::grade;
                    e.clip = cid;
                    e.key = key;
                    e.to = key == MagicGradeKey::exposure ? std::clamp(*v, -2.0, 2.0) : key == MagicGradeKey::contrast ? std::clamp(*v, -50.0, 50.0) : std::clamp(*v, -100.0, 100.0);
                    edits.push_back(e);
                }
            }
            auto intensity = number(args, "look_intensity"), temp = number(args, "temperature"), tint = number(args, "tint");
            if (intensity || temp || tint) {
                edits.push_back(MagicEdit::change(cid, QStringLiteral("Colour"), [=](Project& q) {
                    auto i = q.index(cid);
                    if (!i) return;
                    ColorGrade g = q.clips[*i].grade.value_or(ColorGrade());
                    if (intensity) g.lookIntensity = std::clamp(*intensity, 0.0, 1.0);
                    if (temp) g.temperature = std::clamp(*temp, -50.0, 50.0);
                    if (tint) g.tint = std::clamp(*tint, -50.0, 50.0);
                    q.clips[*i].grade = g;
                }));
            }
        }
        if (edits.empty()) throw MCPToolError{QStringLiteral("Nothing to change: give clip_ids and at least one setting.")};
        perform(edits, m, draft);
        return jsonOut(clipsJSON(done, now()));
    }
    if (n == "set_audio") {
        const Clip& c = clipArg(args, p);
        std::vector<MagicEdit> edits;
        if (auto g = number(args, "gain_db")) { MagicEdit e; e.kind = MagicEdit::Kind::gain; e.clip = c.id; e.from = c.gainDB; e.to = std::clamp(*g, -60.0, 12.0); edits.push_back(e); }
        if (auto f = number(args, "fade_in")) { MagicEdit e; e.kind = MagicEdit::Kind::fadeIn; e.clip = c.id; e.to = std::max(0.0, std::min(*f, c.duration)); edits.push_back(e); }
        if (auto f = number(args, "fade_out")) { MagicEdit e; e.kind = MagicEdit::Kind::fadeOut; e.clip = c.id; e.to = std::max(0.0, std::min(*f, c.duration)); edits.push_back(e); }
        if (edits.empty()) throw MCPToolError{QStringLiteral("Give gain_db, fade_in or fade_out.")};
        perform(edits, m, draft);
        return jsonOut(clipsJSON({c.id}, now()));
    }
    if (n == "set_transition") {
        const Clip& c = clipArg(args, p);
        const QString kindName = args.value("kind").toString(QStringLiteral("dissolve"));
        const Uuid id = c.id;
        if (kindName == QLatin1String("none")) {
            perform({MagicEdit::change(id, QStringLiteral("No transition"), [id](Project& q) { if (auto i = q.index(id)) q.clips[*i].transitionIn.reset(); })}, m, draft);
        } else {
            TransitionKind kind;
            if (kindName == QLatin1String("dissolve")) kind = TransitionKind::dissolve;
            else if (kindName == QLatin1String("dipToBlack")) kind = TransitionKind::dipToBlack;
            else if (kindName == QLatin1String("wipe")) kind = TransitionKind::wipe;
            else throw MCPToolError{QStringLiteral("kind is dissolve, dipToBlack, wipe or none.")};
            if (std::none_of(p.clips.begin(), p.clips.end(), [&](const Clip& o) { return o.trackID == c.trackID && o.id != c.id && std::abs(o.end() - c.start) < 1e-3; }))
                throw MCPToolError{QStringLiteral("Nothing ends where this clip starts on its track, so there's nothing to transition from.")};
            MagicEdit e;
            e.kind = MagicEdit::Kind::transition;
            e.clip = id;
            e.transition = Transition{kind, std::max(0.04, number(args, "duration").value_or(0.5)), {}};
            perform({e}, m, draft);
        }
        return jsonOut(clipsJSON({id}, now()));
    }
    if (n == "add_track") {
        const TrackKind kind = args.value("kind").toString() == QLatin1String("audio") ? TrackKind::audio : TrackKind::video;
        if (draft) draft->apply({addTrackEdit(kind, newUuid())}); else m.addTrack(kind);
        return text(QStringLiteral("Added %1%2.").arg(kind == TrackKind::video ? QStringLiteral("V") : QStringLiteral("A")).arg(now().tracks(kind).size()));
    }
    if (n == "set_sequence") {
        const QJsonValue w = args.value("width"), h = args.value("height");
        std::optional<std::pair<int, int>> size;
        if (w.isDouble() && h.isDouble()) {
            const int wi = w.toInt(), hi = h.toInt();
            if (wi < 16 || hi < 16 || wi > 8192 || hi > 8192) throw MCPToolError{QStringLiteral("Width and height are 16…8192.")};
            size = std::make_pair(wi, hi);
        }
        std::optional<double> fps;
        if (auto f = number(args, "fps")) fps = EditorModel::standardFPS(*f);
        if (draft) {
            draft->apply({MagicEdit::change(std::nullopt, QStringLiteral("Sequence"), [=](Project& q) {
                if (size) { q.width = size->first; q.height = size->second; }
                if (fps) q.fps = *fps;
                q.settingsLocked = true;
            })});
        } else {
            if (size) m.setSequence(size->first, size->second);
            if (fps) m.setFPS(*fps);
        }
        const Project& q = now();
        return jsonOut(QJsonObject{{"width", q.width}, {"height", q.height}, {"fps", q.fps}});
    }
    if (n == "playback") {
        if (draft) {
            // Nothing plays during a Magic edit; seeking moves get_frame's default time.
            if (args.value("action").toString() == QLatin1String("seek")) draft->playhead = std::max(0.0, numArg(args, "time"));
            return text(QStringLiteral("Playhead at %1s. Playback is off while making a Magic edit; use get_frame to see the cut.").arg(QString::asprintf("%.3f", draft->playhead)));
        }
        const QString action = args.value("action").toString();
        if (action == QLatin1String("play")) m.play();
        else if (action == QLatin1String("pause")) m.pause();
        else if (action == QLatin1String("seek")) m.seek(std::max(0.0, numArg(args, "time")));
        else throw MCPToolError{QStringLiteral("action is play, pause or seek.")};
        return text(QStringLiteral("Playhead at %1s, %2.").arg(QString::asprintf("%.3f", m.playhead()), m.isPlaying() ? QStringLiteral("playing") : QStringLiteral("paused")));
    }
    if (n == "history") {
        if (draft) {
            const QString a = args.value("action").toString();
            if (a == QLatin1String("undo")) { if (draft->undoStack.empty()) throw MCPToolError{QStringLiteral("Nothing to undo.")}; draft->undo(); }
            else if (a == QLatin1String("redo")) { if (draft->redoStack.empty()) throw MCPToolError{QStringLiteral("Nothing to redo.")}; draft->redo(); }
            else throw MCPToolError{QStringLiteral("action is undo or redo.")};
            return text(QStringLiteral("Done. %1 clips on the timeline.").arg(draft->project.clips.size()));
        }
        const QString action = args.value("action").toString();
        if (action == QLatin1String("undo")) { if (!m.canUndo()) throw MCPToolError{QStringLiteral("Nothing to undo.")}; m.undo(); }
        else if (action == QLatin1String("redo")) { if (!m.canRedo()) throw MCPToolError{QStringLiteral("Nothing to redo.")}; m.redo(); }
        else throw MCPToolError{QStringLiteral("action is undo or redo.")};
        return text(QStringLiteral("Done. %1 clips on the timeline.").arg(m.project().clips.size()));
    }
    if (n == "auto_edit") notInMagic();
    if (n == "auto_edit") {
        // Local Magic with no request: a new cut from the library, or the edit there improved.
        LocalMagic& local = LocalMagic::of(ctl);
        QEventLoop loop;
        while (local.busy()) { QTimer::singleShot(50, &loop, &QEventLoop::quit); loop.exec(); }
        bool done = false;
        local.start(LocalRecipe{}, [&](const QString&, bool) { done = true; loop.quit(); });
        if (!done) loop.exec();
        return jsonOut(projectJSON(m.project(), m.title(), m.playhead()));
    }
    if (n == "save_project") {
        notInMagic();
        if (m.fileName().isEmpty()) throw MCPToolError{QStringLiteral("This project has never been saved; ask the person to save it (File > Save).")};
        m.save();
        return text(QStringLiteral("Saved."));
    }
    if (n == "export") {
        if (draft) throw MCPToolError{QStringLiteral("export isn't available while making a Magic edit; the person can export it when it's done.")};
        m.commitLive();
        const Project& q = m.project();
        if (q.clips.empty()) throw MCPToolError{QStringLiteral("The timeline is empty.")};
        ExportSettings s;
        if (args.value("format").toString() == QLatin1String("audio")) s.format = ExportSettings::Format::wav;
        if (args.value("shape").isString()) {
            const QString sh = args.value("shape").toString();
            if (sh == QLatin1String("original")) s.shape = ExportSettings::Shape::original;
            else if (sh == QLatin1String("vertical")) s.shape = ExportSettings::Shape::vertical;
            else if (sh == QLatin1String("square")) s.shape = ExportSettings::Shape::square;
            else if (sh == QLatin1String("portrait")) s.shape = ExportSettings::Shape::portrait;
            else throw MCPToolError{QStringLiteral("shape is original, vertical, square or portrait.")};
        }
        if (args.value("height").isDouble()) {
            const int h = args.value("height").toInt();
            if (h < 144 || h > 4320) throw MCPToolError{QStringLiteral("height is 144…4320.")};
            s.height = h;
        }
        if (args.value("codec").toString() == QLatin1String("hevc")) s.codec = ExportSettings::Codec::hevc;
        if (args.value("quality").toString() == QLatin1String("web")) s.quality = ExportSettings::Quality::web;
        if (args.value("include_audio").isBool()) s.includeAudio = args.value("include_audio").toBool();
        if (auto l = number(args, "loudness")) {
            if (*l < -40 || *l > -5) throw MCPToolError{QStringLiteral("loudness is a LUFS target, -40…-5 (like -14).")};
            s.loudness = *l;
        }
        if (args.value("range_only").toBool()) {
            if (!q.playRange()) throw MCPToolError{QStringLiteral("There's no in/out range; set_in_out sets one.")};
            s.rangeOnly = true;
        }
        s.reframeX = std::clamp(number(args, "reframe_x").value_or(0.5), 0.0, 1.0);
        s.reframeY = std::clamp(number(args, "reframe_y").value_or(0.5), 0.0, 1.0);
        if (s.format == ExportSettings::Format::wav && std::none_of(q.clips.begin(), q.clips.end(), [](const Clip& c) { return c.kind == ClipKind::audio; }))
            throw MCPToolError{QStringLiteral("There's no sound on the timeline.")};
        // Where the person last exported to, else beside the project, else Videos.
        QString folder;
        if (q.exportDir && QFileInfo(*q.exportDir).isDir()) folder = *q.exportDir;
        else if (!m.fileName().isEmpty()) folder = QFileInfo(m.fileName()).absolutePath();
        else folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        QString base = (args.value("name").isString() ? args.value("name").toString() : q.exportName.value_or(m.title())).trimmed();
        base.replace(u'/', u'-').replace(u':', u'-');
        if (base.isEmpty()) base = m.title();
        QString path = folder + u'/' + base + u'.' + s.fileExtension();
        auto taken = [&](const QString& pth) {
            if (QFileInfo::exists(pth)) return true;
            for (const auto& i : ctl.exports().items()) if (i.path == pth) return true;
            return false;
        };
        for (int k = 2; taken(path); ++k) path = folder + u'/' + QStringLiteral("%1 %2.").arg(base).arg(k) + s.fileExtension();
        ctl.exports().add(q, s, path);
        const auto [w, h] = s.renderSize(s.output(q));
        QJsonObject out{{"file", path}, {"size", s.format == ExportSettings::Format::wav ? QStringLiteral("audio") : QStringLiteral("%1×%2").arg(w).arg(h)}, {"status", "started"}};
        if (args.value("wait").toBool()) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 240000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                const ExportQueue::Item* item = nullptr;
                for (const auto& i : ctl.exports().items()) if (i.path == path) item = &i;
                if (!item) break;
                if (item->status == ExportQueue::Status::done) { out.insert("status", "done"); return jsonOut(out); }
                if (item->status == ExportQueue::Status::failed) throw MCPToolError{QStringLiteral("The export failed: %1").arg(item->message)};
                if (item->status == ExportQueue::Status::cancelled) { out.insert("status", "cancelled"); return jsonOut(out); }
            }
            out.insert("status", "still exporting; export_status says when it's done");
        }
        return jsonOut(out);
    }
    if (n == "export_status") {
        QJsonArray items;
        for (const auto& i : ctl.exports().items()) {
            QString status;
            switch (i.status) {
            case ExportQueue::Status::waiting: status = QStringLiteral("waiting"); break;
            case ExportQueue::Status::running: status = QStringLiteral("running"); break;
            case ExportQueue::Status::done: status = QStringLiteral("done"); break;
            case ExportQueue::Status::failed: status = QStringLiteral("failed: ") + i.message; break;
            case ExportQueue::Status::cancelled: status = QStringLiteral("cancelled"); break;
            }
            QJsonObject d{{"file", i.path}, {"status", status}};
            if (i.status == ExportQueue::Status::running) d.insert("progress", QString::asprintf("%.2f", ctl.exports().progress()).toDouble());
            items.append(d);
        }
        if (items.isEmpty()) return jsonOut(QJsonObject{{"exports", QJsonArray()}, {"note", "Nothing exporting, and nothing finished recently."}});
        return jsonOut(QJsonObject{{"exports", items}});
    }
    if (n == "search_media") {
        const QString shows = args.value("shows").toString().trimmed().toLower();
        const QString said = args.value("said").toString().trimmed();
        if (shows.isEmpty() && said.isEmpty()) throw MCPToolError{QStringLiteral("Give shows and/or said.")};
        static const QStringList skip = {"a", "an", "the", "of", "in", "on", "at", "with"};
        QStringList words;
        for (const QString& w : shows.split(u' ', Qt::SkipEmptyParts)) if (!skip.contains(w)) words << w;
        QJsonArray found;
        int unseen = 0, unheard = 0;
        for (const auto& a : p.assets) {
            QJsonObject hit{{"asset_id", uuid(a.id)}, {"name", a.name}};
            if (!words.isEmpty()) {
                const auto tags = ctl.sceneTags(a.id);
                if (!tags && a.kind != MediaKind::audio) ++unseen;
                if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return a.name.toLower().contains(w) || (tags && tags->shows(w)); })) continue;
                QJsonArray shown;
                for (const QString& w : words)
                    if (tags) for (const auto& g : tags->matching(w)) {
                        QJsonArray times;
                        for (size_t i = 0; i < g.times.size() && i < 8; ++i) times.append(r3(g.times[i]));
                        shown.append(QJsonObject{{"tag", g.name}, {"times", times}});
                    }
                if (!shown.isEmpty()) hit.insert("shows", shown);
            }
            if (!said.isEmpty()) {
                if (a.kind == MediaKind::image) continue;
                auto t = ctl.media().transcript(a.id);
                if (!t) { ++unheard; continue; }
                const auto places = t->occurrences(said);
                if (places.empty()) continue;
                QJsonArray at;
                for (size_t i = 0; i < places.size() && i < 10; ++i)
                    at.append(QJsonObject{{"start", r3(places[i].start)}, {"end", r3(places[i].end)}, {"words", places[i].context}});
                hit.insert("said", at);
            }
            found.append(hit);
        }
        QJsonObject out{{"found", found}, {"times", "Seconds in each file, like source_start."}};
        QStringList notes;
        if (unseen > 0) notes << QStringLiteral("%1 item%2 been looked through yet").arg(unseen).arg(unseen == 1 ? QStringLiteral(" hasn't") : QStringLiteral("s haven't"));
        if (unheard > 0) notes << QStringLiteral("%1 haven't been heard yet (get_transcript hears one now)").arg(unheard);
        if (!notes.isEmpty()) out.insert("note", notes.join(QStringLiteral("; ")) + u'.');
        return jsonOut(out);
    }
    if (n == "set_marker") {
        std::optional<Marker::Tint> tint;
        if (args.value("color").isString()) {
            static const QStringList names = {"white", "yellow", "green", "blue", "purple", "red"};
            const int i = int(names.indexOf(args.value("color").toString()));
            if (i < 0) throw MCPToolError{QStringLiteral("color is ") + names.join(QStringLiteral(", ")) + u'.'};
            tint = Marker::Tint(i);
        }
        std::optional<double> time;
        if (auto t = number(args, "time")) time = p.snapFrame(std::max(0.0, *t));
        std::optional<QString> label;
        if (args.value("name").isString()) label = args.value("name").toString();
        Uuid id = newUuid();
        if (args.value("marker_id").isString()) {
            const QString s = args.value("marker_id").toString().toUpper();
            const auto list = p.markerList();
            auto it = std::find_if(list.begin(), list.end(), [&](const Marker& mk) { return uuid(mk.id) == s; });
            if (it == list.end()) throw MCPToolError{QStringLiteral("No marker %1; get_project lists them.").arg(args.value("marker_id").toString())};
            id = it->id;
            perform({MagicEdit::change(std::nullopt, QStringLiteral("Marker"), [=](Project& q) {
                if (!q.markers) return;
                for (auto& mk : *q.markers) {
                    if (mk.id != id) continue;
                    if (time) mk.time = *time;
                    if (label) mk.name = *label;
                    if (tint) mk.tint = *tint;
                }
            })}, m, draft);
        } else {
            if (!time) throw MCPToolError{QStringLiteral("time is needed for a new marker.")};
            Marker mk;
            mk.id = id;
            mk.time = *time;
            mk.name = label.value_or(QStringLiteral("Marker %1").arg(p.markers ? p.markers->size() + 1 : 1));
            mk.tint = tint.value_or(Marker::Tint::yellow);
            perform({MagicEdit::change(std::nullopt, QStringLiteral("Marker"), [mk](Project& q) {
                auto list = q.markers.value_or(std::vector<Marker>{});
                list.push_back(mk);
                q.markers = list;
            })}, m, draft);
        }
        for (const auto& mk : now().markerList())
            if (mk.id == id) return jsonOut(QJsonObject{{"id", uuid(mk.id)}, {"time", r3(mk.time)}, {"name", mk.name}, {"color", tintName(mk.tint)}});
        throw MCPToolError{QStringLiteral("The marker didn't stick.")};
    }
    if (n == "delete_markers") {
        std::set<Uuid> ids;
        if (args.value("all").toBool()) { for (const auto& mk : p.markerList()) ids.insert(mk.id); }
        else for (const auto& v : args.value("marker_ids").toArray()) if (auto u = json::parseUuid(v.toString())) ids.insert(*u);
        if (ids.empty()) throw MCPToolError{QStringLiteral("Give marker_ids, or all: true.")};
        perform({MagicEdit::change(std::nullopt, QStringLiteral("Markers"), [ids](Project& q) {
            if (!q.markers) return;
            std::erase_if(*q.markers, [&](const Marker& mk) { return ids.count(mk.id) > 0; });
            if (q.markers->empty()) q.markers.reset();
        })}, m, draft);
        const size_t left = now().markerList().size();
        return text(QStringLiteral("%1 marker%2 left.").arg(left).arg(left == 1 ? QString() : QStringLiteral("s")));
    }
    if (n == "set_in_out") {
        if (args.value("clear").toBool()) {
            perform({MagicEdit::change(std::nullopt, QStringLiteral("Clear in/out"), [](Project& q) { q.inPoint.reset(); q.outPoint.reset(); })}, m, draft);
            return text(QStringLiteral("No in/out range."));
        }
        std::optional<double> i, o;
        if (auto v = number(args, "in")) i = p.snapFrame(std::max(0.0, *v));
        if (auto v = number(args, "out")) o = p.snapFrame(std::max(0.0, *v));
        if (!i && !o) throw MCPToolError{QStringLiteral("Give in and/or out, or clear: true.")};
        if (i && o && *o <= *i) throw MCPToolError{QStringLiteral("out has to be after in.")};
        perform({MagicEdit::change(std::nullopt, QStringLiteral("In/out"), [=](Project& q) {
            if (i) { q.inPoint = *i; if (q.outPoint && *q.outPoint <= *i) q.outPoint.reset(); }
            if (o) { q.outPoint = *o; if (q.inPoint && *q.inPoint >= *o) q.inPoint.reset(); }
        })}, m, draft);
        const auto r = now().playRange();
        return jsonOut(QJsonObject{{"in", r ? QJsonValue(r3(r->start)) : QJsonValue()}, {"out", r ? QJsonValue(r3(r->end)) : QJsonValue()}});
    }
    if (n == "set_transform") {
        const Clip& c = clipArg(args, p);
        if (!c.isTransformable()) throw MCPToolError{c.kind == ClipKind::text ? QStringLiteral("Titles are placed with set_title_style.") : QStringLiteral("Sound has no picture to move.")};
        ClipTransform tf = args.value("reset").toBool() ? ClipTransform() : c.transform.value_or(ClipTransform());
        if (auto s = number(args, "scale")) tf.scale = std::clamp(*s, ClipTransform::scaleMin, ClipTransform::scaleMax);
        if (auto x = number(args, "x")) tf.x = std::clamp(*x, -2.0, 2.0);
        if (auto y = number(args, "y")) tf.y = std::clamp(*y, -2.0, 2.0);
        // Zoom: from the full frame to this framing over the clip (as the Scale field's zoom button).
        if (args.value("zoom").isString()) {
            const QString z = args.value("zoom").toString();
            if (z == QLatin1String("off")) {
                tf.zoom.reset();
            } else if (z == QLatin1String("in") || z == QLatin1String("in_out") || z == QLatin1String("out")) {
                const auto mode = z == QLatin1String("in") ? ZoomMove::Mode::zoomIn : z == QLatin1String("out") ? ZoomMove::Mode::zoomOut : ZoomMove::Mode::inOut;
                const double length = c.duration;
                if (!number(args, "scale") && tf.scale <= 1.01) tf.scale = 2;
                ZoomMove move = ZoomMove::starter(mode, length);
                const auto secs = number(args, "zoom_seconds");
                const double mv = secs ? std::min(std::max(*secs, 0.05), mode == ZoomMove::Mode::inOut ? length / 2 : length)
                                       : (move.p2 - move.p1 > 0 ? move.p2 - move.p1 : move.p4 - move.p3);
                const auto start = number(args, "zoom_start");
                switch (mode) {
                case ZoomMove::Mode::zoomIn:
                    move.p1 = start.value_or(0); move.p2 = move.p1 + mv; move.p3 = length; move.p4 = length; break;
                case ZoomMove::Mode::zoomOut:
                    move.p1 = 0; move.p2 = 0; move.p3 = start.value_or(length - mv); move.p4 = move.p3 + mv; break;
                case ZoomMove::Mode::inOut:
                    move.p1 = start.value_or(0); move.p2 = move.p1 + mv; move.p3 = std::max(move.p2, length - mv); move.p4 = length; break;
                }
                if (args.value("zoom_smooth").isBool()) move.smooth = args.value("zoom_smooth").toBool();
                move.fit(length);
                tf.zoom = move;
            } else {
                throw MCPToolError{QStringLiteral("zoom is in, in_out, out or off.")};
            }
        } else if (args.value("zoom_smooth").isBool() && tf.zoom) {
            tf.zoom->smooth = args.value("zoom_smooth").toBool();
        }
        // A point to frame: the picture is moved so that point is centred (kept within its edges when it fills).
        if (auto fx = number(args, "focus_x")) tf.x = std::clamp(-(std::clamp(*fx, 0.0, 1.0) - 0.5) * tf.scale, -2.0, 2.0);
        if (auto fy = number(args, "focus_y")) tf.y = std::clamp(-(std::clamp(*fy, 0.0, 1.0) - 0.5) * tf.scale, -2.0, 2.0);
        if (tf.fills()) std::tie(tf.x, tf.y) = tf.zoomedOffset();
        const Uuid id = c.id;
        perform({MagicEdit::change(id, QStringLiteral("Transform"), [id, tf](Project& q) {
            if (auto i = q.index(id)) { if (tf.isIdentity()) q.clips[*i].transform.reset(); else q.clips[*i].transform = tf; }
        })}, m, draft);
        return jsonOut(clipsJSON({id}, now()));
    }
    if (n == "set_title_style") {
        const Clip& c = clipArg(args, p);
        if (c.kind != ClipKind::text) throw MCPToolError{QStringLiteral("That clip isn't a title.")};
        std::optional<QString> font;
        if (args.value("font").isString()) {
            const QString f = args.value("font").toString().trimmed();
            if (f.isEmpty() || f.toLower() == QLatin1String("system")) font = QString();
            else {
                const QStringList fams = QFontDatabase::families();
                auto it = std::find_if(fams.begin(), fams.end(), [&](const QString& x) { return x.compare(f, Qt::CaseInsensitive) == 0; });
                if (it == fams.end()) throw MCPToolError{QStringLiteral("%1 isn't installed on this computer. Try System, DejaVu Serif, Noto Sans or Liberation Serif.").arg(f)};
                font = *it;
            }
        }
        const auto color = colorArg(args.value("color"));
        std::optional<QString> words;
        if (args.value("text").isString()) words = args.value("text").toString();
        std::optional<double> size, x, y;
        if (auto v = number(args, "size")) size = std::clamp(*v, 12.0, 600.0);
        if (auto v = number(args, "x")) x = std::clamp(*v, 0.0, 1.0);
        if (auto v = number(args, "y")) y = std::clamp(*v, 0.0, 1.0);
        std::optional<bool> bold;
        if (args.value("bold").isBool()) bold = args.value("bold").toBool();
        if (!font && !color && !words && !size && !bold && !x && !y) throw MCPToolError{QStringLiteral("Give at least one of text, font, size, bold, color, x, y.")};
        const Uuid id = c.id;
        perform({MagicEdit::change(id, QStringLiteral("Title style"), [=](Project& q) {
            auto i = q.index(id);
            if (!i) return;
            TextStyle& t = q.clips[*i].text;
            if (words) t.string = *words;
            if (font) t.fontName = *font;
            if (color) t.color = *color;
            if (size) t.size = *size;
            if (bold) t.bold = *bold;
            if (x) t.x = *x;
            if (y) t.y = *y;
        })}, m, draft);
        return jsonOut(clipsJSON({id}, now()));
    }
    if (n == "freeze_frame") {
        const Clip& c = clipArg(args, p);
        if (c.kind != ClipKind::video || c.isFreeze()) throw MCPToolError{QStringLiteral("Freeze frames are made from a video clip.")};
        const double t = p.snapFrame(number(args, "at").value_or(c.start + c.duration / 2));
        if (!(t > c.start && t < c.end())) throw MCPToolError{QStringLiteral("at has to be inside the clip (%1…%2).").arg(json::formatNumber(r3(c.start)), json::formatNumber(r3(c.end())))};
        const double hold = std::max(1 / p.fps, number(args, "seconds").value_or(2));
        double source = c.sourceTime(t);
        if (c.playsBackwards()) if (const MediaAsset* a = p.asset(c.assetID)) source = std::max(0.0, a->duration - source - 0.001);
        const Uuid made = newUuid();
        const Clip cc = c;
        perform({MagicEdit::change(c.id, QStringLiteral("Freeze"), [=](Project& q) {
            const IdSet group = q.linked({cc.id});
            q.split(group, t);
            std::set<Uuid> tracks;
            for (const auto& x : q.clips) if (group.count(x.id)) tracks.insert(x.trackID);
            IdSet shifting;
            for (const auto& x : q.clips) if (tracks.count(x.trackID) && x.start >= t - 1e-6) shifting.insert(x.id);
            shifting = q.unlocked(q.linked(shifting));
            for (auto& x : q.clips) if (shifting.count(x.id)) x.start += hold;
            Clip f(cc.trackID, ClipKind::video, t, hold, source);
            f.id = made;
            f.assetID = cc.assetID;
            f.freeze = true;
            f.grade = cc.grade;
            f.transform = cc.transform;
            q.clips.push_back(f);
            IdSet keep = shifting;
            keep.insert(made);
            q.resolveOverlaps(keep);
        })}, m, draft);
        return jsonOut(clipsJSON({made}, now()));
    }
    if (n == "reverse_clip") {
        const Clip& c = clipArg(args, p);
        if (!c.playsSource() || c.reversedFrom) throw MCPToolError{QStringLiteral("Only video and sound from a file can be reversed.")};
        const IdSet group = p.unlocked(p.linked({c.id}));
        const bool backwards = !c.playsBackwards();
        if (backwards) for (const auto& x : p.clips) if (group.count(x.id) && x.kind == ClipKind::audio && x.assetID) ctl.prepareReversedAudio(*x.assetID);
        perform({MagicEdit::change(c.id, backwards ? QStringLiteral("Reverse") : QStringLiteral("Forwards"), [=](Project& q) {
            for (auto& x : q.clips) {
                if (!group.count(x.id) || !x.playsSource() || x.reversedFrom) continue;
                const MediaAsset* a = q.asset(x.assetID);
                if (!a) continue;
                const Clip old = x;
                x.sourceStart = std::max(0.0, a->duration - (old.sourceStart + old.sourceLength()));
                x.ramp = old.ramp ? std::optional<SpeedRamp>(old.ramp->mirrored(a->duration)) : std::nullopt;
                x.reversed = backwards ? std::optional<bool>(true) : std::nullopt;
            }
        })}, m, draft);
        return jsonOut(clipsJSON({group.begin(), group.end()}, now()));
    }
    if (n == "detach_audio") {
        const Clip& c = clipArg(args, p);
        if (!c.linkID) throw MCPToolError{QStringLiteral("That clip isn't linked to anything.")};
        const IdSet group = p.linked({c.id});
        perform({MagicEdit::change(c.id, QStringLiteral("Detach"), [group](Project& q) { q.unlink(group); })}, m, draft);
        return jsonOut(clipsJSON({group.begin(), group.end()}, now()));
    }
    if (n == "duplicate_clip") {
        const Clip& c = clipArg(args, p);
        const IdSet ids = p.linked({c.id});
        std::vector<Clip> group;
        for (const auto& x : p.clips) if (ids.count(x.id)) group.push_back(x);
        double t0 = c.start, t1 = c.end();
        for (const auto& x : group) { t0 = std::min(t0, x.start); t1 = std::max(t1, x.end()); }
        const double at = p.snapFrame(std::max(0.0, number(args, "start").value_or(t1)));
        std::map<Uuid, Uuid> links;
        std::vector<Clip> copies;
        for (const auto& original : group) {
            Clip x = original;
            x.id = newUuid();
            if (x.linkID) { if (!links.count(*x.linkID)) links[*x.linkID] = newUuid(); x.linkID = links[*x.linkID]; }
            x.start = at + (original.start - t0);
            copies.push_back(x);
        }
        perform({MagicEdit::place(copies, c.assetID)}, m, draft);
        std::vector<Uuid> made;
        for (const auto& x : copies) made.push_back(x.id);
        return jsonOut(clipsJSON(made, now()));
    }
    if (n == "set_track") {
        if (!args.value("track").isString()) throw MCPToolError{QStringLiteral("track is needed (V1, A1…).")};
        const QString tn = args.value("track").toString();
        const QString up = tn.toUpper().trimmed();
        const TrackKind kind = up.startsWith(u'V') ? TrackKind::video : TrackKind::audio;
        bool ok = false;
        const int i = up.mid(1).toInt(&ok);
        if (!ok || i < 1 || i > int(p.tracks(kind).size())) throw MCPToolError{QStringLiteral("There's no %1.").arg(tn)};
        const Uuid id = p.tracks(kind)[size_t(i - 1)].id;
        auto opt = [&](const char* k) { return args.value(QLatin1String(k)).isBool() ? std::optional<bool>(args.value(QLatin1String(k)).toBool()) : std::nullopt; };
        const auto hidden = opt("hidden"), muted = opt("muted"), solo = opt("solo"), locked = opt("locked");
        std::optional<double> gain;
        if (auto g = number(args, "gain_db")) gain = std::clamp(*g, -60.0, 12.0);
        if (!hidden && !muted && !solo && !locked && !gain) throw MCPToolError{QStringLiteral("Give hidden, muted, solo, locked or gain_db.")};
        perform({MagicEdit::change(std::nullopt, QStringLiteral("Track"), [=](Project& q) {
            q.updateTrack(id, [&](Track& t) {
                if (hidden) t.hidden = *hidden;
                if (muted) t.muted = *muted;
                if (solo) t.solo = *solo;
                if (locked) t.locked = *locked;
                if (gain) t.gainDB = *gain;
            });
        })}, m, draft);
        const Track& t = now().tracks(kind)[size_t(i - 1)];
        return jsonOut(QJsonObject{{"track", up}, {"hidden", t.hidden}, {"muted", t.muted}, {"solo", t.solo}, {"locked", t.locked}, {"gain_db", r3(t.gainDB)}});
    }
    if (n == "add_solid") {
        const RGBA color = colorArg(args.value("color")).value_or(RGBA::black());
        const double start = p.snapFrame(std::max(0.0, number(args, "start").value_or(0)));
        const double length = p.snapFrame(std::max(1 / p.fps, number(args, "duration").value_or(3)));
        std::vector<MagicEdit> edits;
        Uuid track;
        if (args.value("track").isString()) {
            track = trackArg(args.value("track").toString(), p, TrackKind::video);
        } else {
            std::optional<Uuid> free;
            for (const auto& t : p.videoTracks) {
                if (t.locked) continue;
                if (std::none_of(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return c.trackID == t.id && c.start < start + length && c.end() > start; })) { free = t.id; break; }
            }
            if (free) track = *free;
            else { track = newUuid(); edits.push_back(addTrackEdit(TrackKind::video, track)); }
        }
        Clip solid(track, ClipKind::solid, start, length);
        solid.color = color;
        edits.push_back(MagicEdit::place({solid}, std::nullopt));
        perform(edits, m, draft);
        return jsonOut(clipsJSON({solid.id}, now()));
    }
    if (n == "set_solid_color") {
        const Clip& c = clipArg(args, p);
        if (c.kind != ClipKind::solid) throw MCPToolError{QStringLiteral("That clip isn't a solid.")};
        auto color = colorArg(args.value("color"));
        if (!color) throw MCPToolError{QStringLiteral("color is needed, like #000000.")};
        const Uuid id = c.id;
        const RGBA col = *color;
        perform({MagicEdit::change(id, QStringLiteral("Colour"), [id, col](Project& q) { if (auto i = q.index(id)) q.clips[*i].color = col; })}, m, draft);
        return jsonOut(clipsJSON({id}, now()));
    }
    throw MCPToolError{QStringLiteral("No tool called %1.").arg(name)};
}

} // namespace er
