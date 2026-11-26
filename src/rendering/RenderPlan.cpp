#include "RenderPlan.h"
#include "model/Codec.h"
#include "model/Json.h"
#include <QFont>
#include <QJsonArray>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <list>

namespace er {

// MARK: PlanLayer (RenderLayer)

double PlanLayer::opacity(double t) const {
    double o = 1.0;
    // The clip's own fades; in the extra a transition shows, they hold their value at that end.
    const double ft = std::min(std::max(t, fadeStart), fadeEnd);
    if (fadeIn > 0) o = std::min(o, applyFadeCurve(inCurve, (ft - fadeStart) / fadeIn));
    if (fadeOut > 0) o = std::min(o, applyFadeCurve(outCurve, (fadeEnd - ft) / fadeOut));
    if (reveal && reveal->kind != TransitionKind::wipe) o *= reveal->progress(t);
    if (conceal) o *= 1 - conceal->progress(t);
    return std::max(0.0, std::min(1.0, o));
}

std::optional<double> PlanLayer::wipe(double t) const {
    if (!reveal || reveal->kind != TransitionKind::wipe || !(t < reveal->end)) return std::nullopt;
    const double p = reveal->progress(t);
    return p * p * (3 - 2 * p);
}

double PlanLayer::sourceTime(double t) const {
    if (!clock) return 0;
    const double s = clock->source(t - 1e-4);
    return reversed ? length - s : s;
}

// MARK: Build

std::pair<int, int> CompositionBuilder::previewSize(const Project& project) {
    const double w = project.width, h = project.height;
    const double s = std::min(1.0, 1920 / std::max(w, h));
    return {int(std::max(2.0, std::round(w * s / 2) * 2)), int(std::max(2.0, std::round(h * s / 2) * 2))};
}

static uint64_t fnv(const QByteArray& b, uint64_t h = 1469598103934665603ull) {
    for (char c : b) { h ^= uint8_t(c); h *= 1099511628211ull; }
    return h;
}

std::optional<RenderPlan> CompositionBuilder::build(const Project& project, const MediaInfo& media, const BuildOptions& o) {
    RenderPlan plan;
    plan.outW = o.renderW;
    plan.outH = o.renderH;
    // The sequence is laid out on the canvas, then the offset picks the output's window from it.
    double cw = o.renderW, ch = o.renderH;
    if (o.reframe && o.renderH > 0) {
        const double pa = project.aspect(), oa = double(o.renderW) / o.renderH;
        if (std::abs(pa - oa) > 0.001) {
            if (pa > oa) { cw = std::round(o.renderH * pa); ch = o.renderH; }
            else { cw = o.renderW; ch = std::round(o.renderW / pa); }
            const auto [cx, cy] = *o.reframe;
            plan.offsetX = std::min(std::max(0.0, cx * cw - o.renderW / 2.0), cw - o.renderW);
            // Core Image counts up from the bottom; LX keeps `top` (y down).
            plan.offsetY = std::min(std::max(0.0, cy * ch - o.renderH / 2.0), ch - o.renderH);
        }
    }
    plan.canvasW = int(cw);
    plan.canvasH = int(ch);
    plan.duration = project.duration();
    plan.fps = project.fps;
    if (!(plan.duration > 0)) return std::nullopt;

    for (const auto& track : project.videoTracks) {
        if (track.hidden) continue; // a hidden track adds nothing to the picture
        std::vector<Clip> clips;
        for (const auto& c : project.clips) if (c.trackID == track.id) clips.push_back(c);
        std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        for (const auto& clip : clips) {
            const TransitionPlan tp = project.transitionPlan(clip);
            PlanLayer L;
            L.start = clip.start - tp.leadIn;
            L.end = clip.end() + tp.tailOut;
            L.fadeStart = clip.start;
            L.fadeEnd = clip.end();
            L.reveal = tp.reveal;
            L.conceal = tp.conceal;
            L.fadeIn = clip.fadeIn;
            L.fadeOut = clip.fadeOut;
            L.inCurve = clip.inCurve();
            L.outCurve = clip.outCurve();
            if (clip.transform && !clip.transform->isIdentity()) L.transform = clip.transform;
            L.cube = GradeBaker::cube(clip.grade);
            switch (clip.kind) {
            case ClipKind::video: {
                if (!clip.assetID) continue;
                const MediaRuntime* rt = media.runtime(*clip.assetID);
                if (!rt) continue;
                Clip shown = clip;
                shown.start -= tp.leadIn;
                if (clip.playsBackwards()) {
                    // Read backwards from the file as it plays.
                    const MediaAsset* a = project.asset(clip.assetID);
                    if (!a) continue;
                    shown.sourceStart = clip.sourceTime(shown.start);
                    L.reversed = true;
                    L.length = a->duration;
                } else if (!clip.isFreeze()) {
                    shown.sourceStart = clip.sourceTime(shown.start);
                }
                L.kind = PlanLayer::Kind::video;
                L.asset = *clip.assetID;
                L.clock = SourceClock(shown);
                L.orientation = rt->orientation;
                L.color = rt->color;
                break;
            }
            case ClipKind::image: {
                if (!clip.assetID) continue;
                const MediaRuntime* rt = media.runtime(*clip.assetID);
                if (!rt || !rt->still) continue;
                L.kind = PlanLayer::Kind::image;
                L.still = rt->still;
                break;
            }
            case ClipKind::solid:
                L.kind = PlanLayer::Kind::solid;
                L.rgba = clip.color;
                break;
            case ClipKind::text: {
                auto img = TextRenderer::image(clip.text, plan.canvasW, plan.canvasH);
                if (!img) continue;
                L.kind = PlanLayer::Kind::text;
                L.text = img;
                break;
            }
            case ClipKind::audio: continue;
            }
            plan.layers.push_back(std::move(L));
        }
    }
    // Captions: read from what's said in the captioned clips, unless the caption track is hidden.
    if (project.captions && !project.captions->hidden) {
        auto blocks = CaptionLayout::blocks(project, [&](const MediaAsset& a) { return media.transcript(a.id); });
        plan.captions = CaptionRenderer::layers(blocks, project.captions->style);
    }
    QByteArray key = codec::compactBytes(SavedDocument{1, project, {}});
    key += QByteArray::number(o.renderW) + 'x' + QByteArray::number(o.renderH);
    if (o.reframe) key += QByteArray::number(o.reframe->first, 'g', 17) + ',' + QByteArray::number(o.reframe->second, 'g', 17);
    plan.signature = fnv(key);
    return plan;
}

// MARK: Frames

FrameJob describeFrame(const RenderPlan& plan, double t, const FrameProvider& frames) {
    FrameJob job;
    job.time = t;
    const float W = float(plan.canvasW), H = float(plan.canvasH);
    std::map<uint64_t, int> sourceByKey;
    auto addSource = [&](const VideoFrame& f) -> int {
        if (auto it = sourceByKey.find(f.key); it != sourceByKey.end() && f.key) return it->second;
        cm::ErSource s{};
        s.format = f.format;
        s.width = f.width;
        s.height = f.height;
        s.offset0 = f.offset[0]; s.offset1 = f.offset[1]; s.offset2 = f.offset[2];
        s.stride0 = f.stride[0]; s.stride1 = f.stride[1];
        s.matrix = f.matrix;
        s.fullRange = f.fullRange ? 1 : 0;
        s.orientation = f.orientation;
        job.sources.push_back(s);
        job.pictures.push_back(f);
        const int i = int(job.sources.size()) - 1;
        if (f.key) sourceByKey[f.key] = i;
        return i;
    };
    auto addCube = [&](const std::shared_ptr<const std::vector<float>>& cube) -> int {
        if (!cube) return -1;
        for (const auto& [k, off] : job.cubeKeys) if (k == cube.get()) return off;
        const int off = int(job.cubes.size());
        job.cubes.insert(job.cubes.end(), cube->begin(), cube->end());
        job.cubeKeys.emplace_back(cube.get(), off);
        return off;
    };
    auto fit = [&](cm::ErLayer& L, int dw, int dh) {
        const float s = std::min(W / float(dw), H / float(dh));
        L.fitScale = s;
        L.fitX = (W - float(dw) * s) / 2;
        L.fitY = (H - float(dh) * s) / 2;
    };

    for (size_t index = 0; index < plan.layers.size(); ++index) {
        const PlanLayer& layer = plan.layers[index];
        if (!(t >= layer.start && t < layer.end)) continue;
        const double opacity = layer.opacity(t);
        if (opacity <= 0.001) continue;
        cm::ErLayer L{};
        L.gradeCube = -1;
        L.colorCube = -1;
        L.wipe = -1;
        L.scale = 1;
        switch (layer.kind) {
        case PlanLayer::Kind::video: {
            auto f = frames ? frames(int(index), layer, layer.sourceTime(t)) : std::nullopt;
            if (!f || !f->valid()) continue;
            VideoFrame pic = *f;
            pic.orientation = layer.orientation;
            L.kind = ER_LAYER_PICTURE;
            L.source = addSource(pic);
            fit(L, pic.displayWidth(), pic.displayHeight());
            // After scaling, so a 4K source is converted at the output's size.
            L.colorCube = addCube(SourceColors::cube(layer.color));
            break;
        }
        case PlanLayer::Kind::image:
            L.kind = ER_LAYER_PICTURE;
            L.source = addSource(*layer.still);
            fit(L, layer.still->displayWidth(), layer.still->displayHeight());
            break;
        case PlanLayer::Kind::solid: {
            const RGBA& c = layer.rgba;
            L.kind = ER_LAYER_SOLID;
            L.r = float(c.r * c.a); L.g = float(c.g * c.a); L.b = float(c.b * c.a); L.a = float(c.a);
            break;
        }
        case PlanLayer::Kind::text:
            L.kind = ER_LAYER_PIXMAP;
            L.source = addSource(layer.text->image);
            L.pixX = layer.text->x;
            L.pixY = layer.text->y;
            break;
        }
        if (auto w = layer.wipe(t)) L.wipe = float(*w);
        if (layer.transform) {
            if (auto tf = layer.transform->at(t - layer.fadeStart)) {
                // Scale about the frame's centre, then offset (positive y is down, in both).
                L.scale = float(tf->scale);
                L.offsetX = float(tf->x * W);
                L.offsetY = float(tf->y * H);
            }
        }
        L.gradeCube = addCube(layer.cube);
        L.opacity = opacity < 0.999 ? float(opacity) : 1.0f;
        job.layers.push_back(L);
    }
    const int canvasLayers = int(job.layers.size());
    for (const auto& c : plan.captions) {
        if (!(t >= c.start && t < c.end)) continue;
        auto img = CaptionRenderer::image(c.words, c.lit, c.style, plan.outW, plan.outH);
        if (!img) continue;
        cm::ErLayer L{};
        L.kind = ER_LAYER_PIXMAP;
        L.source = addSource(img->image);
        L.pixX = img->x;
        L.pixY = img->y;
        L.opacity = 1;
        L.scale = 1;
        L.wipe = -1;
        L.gradeCube = -1;
        L.colorCube = -1;
        job.layers.push_back(L);
    }
    job.frame.canvasW = plan.canvasW;
    job.frame.canvasH = plan.canvasH;
    job.frame.outW = plan.outW;
    job.frame.outH = plan.outH;
    job.frame.offsetX = float(plan.offsetX);
    job.frame.offsetY = float(plan.offsetY);
    job.frame.layerCount = canvasLayers;
    job.frame.firstCaption = canvasLayers;
    job.frame.captionCount = int(job.layers.size()) - canvasLayers;
    return job;
}

// MARK: Text

namespace {

/// The font for "the system font" (an empty family): Inter, bundled, in place of SF Pro.
QString systemFamily() {
    static const QString family = [] {
        const QStringList fams = QFontDatabase::families();
        for (const char* f : {"Inter", "Inter Display", "Cantarell", "Noto Sans", "DejaVu Sans"})
            if (fams.contains(QLatin1String(f))) return QString::fromLatin1(f);
        return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    }();
    return family;
}

QImage pointImage(int w, int h) {
    QImage img(w, h, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    // 72 dpi, so a point is a pixel, as in a Core Graphics bitmap context.
    img.setDotsPerMeterX(int(std::round(72 / 0.0254)));
    img.setDotsPerMeterY(int(std::round(72 / 0.0254)));
    return img;
}

std::shared_ptr<Pixmap> toPixmap(const QImage& img, int x, int y) {
    auto p = std::make_shared<Pixmap>();
    const QImage c = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::vector<uint8_t> px(size_t(c.width()) * c.height() * 4);
    for (int row = 0; row < c.height(); ++row) std::memcpy(px.data() + size_t(row) * c.width() * 4, c.constScanLine(row), size_t(c.width()) * 4);
    p->image = VideoFrame::rgba(c.width(), c.height(), std::move(px));
    p->x = x;
    p->y = y;
    return p;
}

/// Lines of words laid out as AppKit does for a centred paragraph: greedy word wrapping within
/// `maxWidth`, every line centred. `spaceExtra` widens the spaces (the captions' kern).
struct Line { QStringList words; std::vector<double> x; double width = 0; };
std::vector<Line> layoutWords(const QString& text, const QFontMetricsF& fm, double maxWidth, double spaceExtra) {
    std::vector<Line> lines;
    const double space = fm.horizontalAdvance(QLatin1Char(' ')) + spaceExtra;
    for (const QString& para : text.split(u'\n')) {
        Line line;
        for (const QString& w : para.split(u' ', Qt::SkipEmptyParts)) {
            const double ww = fm.horizontalAdvance(w);
            const double at = line.words.isEmpty() ? 0 : line.width + space;
            if (!line.words.isEmpty() && at + ww > maxWidth) {
                lines.push_back(line);
                line = Line();
                line.words << w;
                line.x.push_back(0);
                line.width = ww;
                continue;
            }
            line.words << w;
            line.x.push_back(at);
            line.width = at + ww;
        }
        lines.push_back(line);
    }
    return lines;
}

struct TextKey {
    QByteArray style;
    int w, h;
    bool operator==(const TextKey& o) const { return style == o.style && w == o.w && h == o.h; }
};
size_t qHash(const TextKey& k, size_t seed = 0) { return ::qHash(k.style, seed) ^ size_t(k.w * 31 + k.h); }

QMutex textLock;
QHash<TextKey, std::shared_ptr<const Pixmap>> textCache;
QHash<TextKey, std::shared_ptr<const Pixmap>> captionCache;

QByteArray styleKey(const QJsonObject& o) { return json::write(o, false); }

} // namespace

std::shared_ptr<const Pixmap> TextRenderer::image(const TextStyle& style, int W, int H) {
    if (style.string.isEmpty() || W <= 0 || H <= 0) return nullptr;
    QJsonObject so{{"s", style.string}, {"f", style.fontName}, {"b", style.bold}, {"z", style.size}, {"x", style.x}, {"y", style.y},
                   {"c", QJsonArray{style.color.r, style.color.g, style.color.b, style.color.a}}};
    const TextKey key{styleKey(so), W, H};
    {
        QMutexLocker l(&textLock);
        if (auto hit = textCache.value(key)) return hit;
    }
    const double pt = style.size * H / 1080.0;
    QFont font(style.fontName.isEmpty() ? systemFamily() : style.fontName);
    font.setPointSizeF(pt);
    font.setWeight(style.bold ? QFont::Bold : QFont::Normal);
    // Metrics at 72 dpi (points are pixels).
    QImage probe = pointImage(1, 1);
    const QFontMetricsF fm(font, &probe);
    const auto lines = layoutWords(style.string, fm, W * 0.94, 0);
    double tw = 0;
    for (const auto& l : lines) tw = std::max(tw, l.width);
    const double lh = fm.lineSpacing();
    const double th = lh * lines.size();
    const double pad = std::ceil(pt * 0.25);
    const int w = int(std::ceil(tw) + pad * 2), h = int(std::ceil(th) + pad * 2);
    if (w <= 0 || h <= 0 || w >= 16384 || h >= 16384) return nullptr;
    QImage img = pointImage(w, h);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font);
        const RGBA& c = style.color;
        p.setPen(QColor::fromRgbF(float(c.r), float(c.g), float(c.b), float(c.a)));
        const double boxW = std::ceil(tw);
        for (size_t i = 0; i < lines.size(); ++i) {
            const double left = pad + (boxW - lines[i].width) / 2;
            const double base = pad + lh * i + fm.ascent();
            for (qsizetype k = 0; k < lines[i].words.size(); ++k)
                p.drawText(QPointF(left + lines[i].x[size_t(k)], base), lines[i].words[k]);
        }
    }
    const double cx = style.x * W;
    const double cyUp = (1 - style.y) * H;
    const int x = int(std::round(cx - w / 2.0));
    const int yUp = int(std::round(cyUp - h / 2.0));
    auto pix = toPixmap(img, x, H - (yUp + h));
    QMutexLocker l(&textLock);
    if (textCache.size() > 64) textCache.clear();
    textCache.insert(key, pix);
    return pix;
}

std::vector<CaptionPlanLayer> CaptionRenderer::layers(const std::vector<CaptionBlock>& blocks, const CaptionStyle& style) {
    std::vector<CaptionPlanLayer> out;
    for (const auto& b : blocks) {
        QStringList words;
        for (const auto& w : b.words) words << w.text;
        if (!style.highlight || words.size() <= 1) {
            out.push_back({b.start, b.end, words, style.highlight ? std::optional<int>(0) : std::nullopt, style});
            continue;
        }
        for (size_t i = 0; i < b.words.size(); ++i) {
            const double start = i == 0 ? b.start : b.words[i].start;
            const double end = i + 1 < b.words.size() ? b.words[i + 1].start : b.end;
            if (!(end > start)) continue;
            out.push_back({start, end, words, int(i), style});
        }
    }
    return out;
}

std::shared_ptr<const Pixmap> CaptionRenderer::image(const QStringList& words, std::optional<int> lit, const CaptionStyle& style, int W, int H) {
    if (words.isEmpty() || W <= 0 || H <= 0) return nullptr;
    auto rgba = [](const RGBA& c) { return QJsonArray{c.r, c.g, c.b, c.a}; };
    QJsonObject so{{"w", QJsonArray::fromStringList(words)}, {"l", lit ? *lit : -1}, {"z", style.size}, {"h", style.highlight},
                   {"t", rgba(style.textColor)}, {"hc", rgba(style.highlightColor)}, {"o", style.outline}, {"oc", rgba(style.outlineColor)}};
    const TextKey key{styleKey(so), W, H};
    std::shared_ptr<const Pixmap> drawn;
    {
        QMutexLocker l(&textLock);
        drawn = captionCache.value(key);
    }
    if (!drawn) {
        // Heavy words in the text colour, with an outline if it has one, the lit one in the highlight colour.
        const double pt = style.size * std::min(W, H) / 1080.0;
        QFont font(systemFamily());
        font.setPointSizeF(pt);
        font.setWeight(QFont::ExtraBold);
        QImage probe = pointImage(1, 1);
        const QFontMetricsF fm(font, &probe);
        // A little wider than a plain space: the outline eats into the gap.
        const auto lines = layoutWords(words.join(u' '), fm, W * 0.86, pt * 0.06);
        double tw = 0;
        for (const auto& l : lines) tw = std::max(tw, l.width);
        const double lh = fm.lineSpacing();
        const double th = lh * lines.size();
        const double pad = std::ceil(pt * 0.3);
        const int w = int(std::ceil(tw) + pad * 2), h = int(std::ceil(th) + pad * 2);
        if (w <= 0 || h <= 0 || w >= 16384 || h >= 16384) return nullptr;
        QImage img = pointImage(w, h);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            auto qc = [](const RGBA& c) { return QColor::fromRgbF(float(c.r), float(c.g), float(c.b), float(c.a)); };
            const double boxW = std::ceil(tw);
            QPainterPath all;
            std::vector<std::pair<QPainterPath, bool>> wordPaths;
            int index = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                const double left = pad + (boxW - lines[i].width) / 2;
                const double base = pad + lh * i + fm.ascent();
                for (qsizetype k = 0; k < lines[i].words.size(); ++k, ++index) {
                    QPainterPath path;
                    path.addText(QPointF(left + lines[i].x[size_t(k)], base), font, lines[i].words[k]);
                    all.addPath(path);
                    wordPaths.emplace_back(path, lit && *lit == index);
                }
            }
            if (style.outline) {
                // AppKit's stroke width 22 is 22% of the point size, centred on the outline.
                QPen pen(qc(style.outlineColor), pt * 0.22, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                p.strokePath(all, pen);
                p.fillPath(all, qc(style.outlineColor));
            }
            for (const auto& [path, isLit] : wordPaths) p.fillPath(path, qc(isLit ? style.highlightColor : style.textColor));
        }
        drawn = toPixmap(img, 0, 0);
        QMutexLocker l(&textLock);
        if (captionCache.size() > 400) captionCache.clear();
        captionCache.insert(key, drawn);
    }
    // Centred, its middle at the style's height, kept inside the frame.
    const int w = drawn->image.width, h = drawn->image.height;
    const double cyUp = (1 - style.y) * H;
    const double yUp = std::min(std::max(cyUp - h / 2.0, 0.0), double(H - h));
    auto placed = std::make_shared<Pixmap>(*drawn);
    placed->x = int(std::round((W - w) / 2.0));
    placed->y = H - (int(std::round(yUp)) + h);
    return placed;
}

} // namespace er
