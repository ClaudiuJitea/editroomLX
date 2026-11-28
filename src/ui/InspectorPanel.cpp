#include "InspectorPanel.h"
#include "InspectorGraphs.h"
#include "InspectorWidgets.h"
#include "Symbols.h"
#include "Theme.h"
#include "model/ColorGrade.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace er {

namespace {

const std::vector<std::tuple<QString, int, int>> kSizes = {
    {"4K  3840 × 2160", 3840, 2160}, {"1440p  2560 × 1440", 2560, 1440}, {"1080p  1920 × 1080", 1920, 1080},
    {"720p  1280 × 720", 1280, 720}, {"Vertical  1080 × 1920", 1080, 1920}, {"Vertical 4K  2160 × 3840", 2160, 3840},
    {"Square  1080 × 1080", 1080, 1080}};

const Clip* findClip(const Project& p, const Uuid& id) {
    auto i = p.index(id);
    return i ? &p.clips[*i] : nullptr;
}

/// The header's icon: an SF Symbol stand-in, or the solid's swatch.
class HeaderIcon : public QWidget {
public:
    explicit HeaderIcon(QWidget* parent) : QWidget(parent) { setFixedSize(18, 18); }
    void set(const QString& s) { symbol_ = s; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (symbol_ == QStringLiteral("solid")) {
            // SolidIcon, 15 × 11: a filled frame.
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::textDim());
            p.drawRoundedRect(QRectF(1.5, 3.5, 15, 11), 2.5, 2.5);
            return;
        }
        Symbols::draw(p, symbol_, QPointF(9, 9), 12, Theme::textDim(), 500);
    }
private:
    QString symbol_;
};

/// The clip-length-style number formats.
QString seconds2(double v) { return QString::asprintf("%.2fs", v); }
QString whole(double v) { return QString::number(qint64(std::round(v))); }
QString signed2(double v) { return QString::asprintf("%+.2f", v); }

} // namespace

InspectorPanel::InspectorPanel(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    setAutoFillBackground(false);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    header_ = new QWidget;
    header_->setFixedHeight(44);
    auto* h = new QHBoxLayout(header_);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(9);
    auto* icon = new HeaderIcon(header_);
    h->addWidget(icon);
    headerTitle_ = new QLabel;
    headerTitle_->setFont(Theme::font(13, 600));
    headerTitle_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::text().name()));
    h->addWidget(headerTitle_, 1);
    v->addWidget(header_);
    scroll_ = new QScrollArea;
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    v->addWidget(scroll_, 1);
    onRefresh([this, icon] {
        const auto clips = selected();
        headerTitle_->setText(title(clips));
        icon->set(headerSymbol(clips));
    });
    connect(&model(), &EditorModel::selectionChanged, this, &InspectorPanel::changed);
    connect(&model(), &EditorModel::changed, this, &InspectorPanel::changed);
    rebuild();
}

void InspectorPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::panel());
}

std::vector<Clip> InspectorPanel::selected() const {
    std::vector<Clip> out;
    for (const auto& c : model().project().clips) if (model().selection().count(c.id)) out.push_back(c);
    return out;
}

/// Captions picked, or the caption track's header clicked.
bool InspectorPanel::showsCaptions() const {
    return model().project().captions && (model().captionsFocused() || !model().captionSelection().empty());
}

static const Clip* primary(const std::vector<Clip>& clips) {
    for (const auto& c : clips) if (c.kind != ClipKind::audio) return &c;
    return clips.empty() ? nullptr : &clips.front();
}

static size_t groups(const std::vector<Clip>& clips) {
    std::set<Uuid> g;
    for (const auto& c : clips) g.insert(c.linkID.value_or(c.id));
    return g.size();
}

QString InspectorPanel::title(const std::vector<Clip>& clips) const {
    if (clips.empty() && showsCaptions()) {
        const size_t n = model().captionSelection().size();
        return n > 1 ? QStringLiteral("%1 captions").arg(n) : n == 1 ? QStringLiteral("Caption") : QStringLiteral("Captions");
    }
    if (clips.empty()) return QStringLiteral("Sequence");
    if (groups(clips) == 1) if (const Clip* c = primary(clips)) return model().project().name(*c);
    return QStringLiteral("%1 clips").arg(clips.size());
}

QString InspectorPanel::headerSymbol(const std::vector<Clip>& clips) const {
    if (!clips.empty() && std::all_of(clips.begin(), clips.end(), [](const Clip& c) { return c.kind == ClipKind::solid; })) return QStringLiteral("solid");
    if (clips.empty() && showsCaptions()) return QStringLiteral("captions.bubble");
    const Clip* c = primary(clips);
    if (!c) return QStringLiteral("rectangle.on.rectangle");
    if (groups(clips) > 1) return QStringLiteral("square.stack");
    switch (c->kind) {
    case ClipKind::video: return QStringLiteral("film");
    case ClipKind::audio: return QStringLiteral("waveform");
    case ClipKind::image: return QStringLiteral("photo");
    case ClipKind::text: return QStringLiteral("textformat");
    case ClipKind::solid: return QStringLiteral("square.fill");
    }
    return {};
}

// MARK: Rebuilding

/// Everything that decides which rows there are (not their values, which are refreshed in place).
QByteArray InspectorPanel::signature() const {
    const EditorModel& m = model();
    const Project& p = m.project();
    QByteArray k;
    auto add = [&k](const QByteArray& b) { k += b; k += '|'; };
    const auto clips = selected();
    if (clips.empty() && showsCaptions()) {
        add("captions");
        std::vector<QString> picked;
        for (const auto& b : m.captionBlocks()) if (m.captionSelection().count(b.id())) picked.push_back(b.id());
        add(QByteArray::number(int(picked.size())));
        if (picked.size() == 1) add(picked.front().toUtf8());
        add(m.captionsWaiting() ? "w" : "-");
        add(c_.transcripts().unsupportedLanguage().toUtf8());
        return k;
    }
    if (clips.empty()) {
        bool hasVideo = false;
        for (const auto& a : p.assets) if (a.kind == MediaKind::video) hasVideo = true;
        add("sequence");
        add(!p.settingsLocked && !hasVideo ? "note" : "-");
        add(p.isHDR() ? "hdr" : "sdr");
        return k;
    }
    add(rampOrigin_ ? "ramping" : "-");
    for (const auto& c : clips) {
        add(c.id.toByteArray());
        add(QByteArray::number(int(c.kind)));
        add(c.isRamped() ? "r" : "-");
        add(std::abs(c.speed - 1) > 0.001 ? "s" : "-");
        add(std::abs(c.gainDB) > 0.001 ? "g" : "-");
        const ClipTransform t = c.transform.value_or(ClipTransform());
        add(t.isIdentity() ? "i" : "-");
        add(t.zoom ? "z" : "-");
        add(t.fills() ? "f" : "-");
        add(std::abs(c.text.x - 0.5) < 1e-6 && std::abs(c.text.y - 0.5) < 1e-6 ? "c" : "-");
        add(c.fadeIn > 0 || c.fadeOut > 0 ? "fade" : "-");
        const ColorGrade g = c.grade.value_or(ColorGrade());
        add(g.isNeutral() ? "n" : "-");
        add(g.off ? "off" : "on");
        add(g.look ? "l" : "-");
        // The transitions either side, when it's the only picture.
        const Clip* before = p.previousTouching(c);
        const Clip* after = p.nextTouching(c);
        add(before ? before->id.toByteArray() : "-");
        add(after ? after->id.toByteArray() + (after->transitionIn ? "t" : "-") : "-");
        add(c.transitionIn ? "t" : "-");
    }
    return k;
}

void InspectorPanel::changed() {
    if (editing_ > 0) {
        pending_ = true;
        for (const auto& f : refreshers_) f();
        return;
    }
    if (signature() != shown_ || !body_) {
        // Rebuilt after the event that changed it (never from inside a row's own handler).
        QTimer::singleShot(0, this, [this] { if (signature() != shown_ || !body_) rebuild(); else for (const auto& f : refreshers_) f(); });
        for (const auto& f : refreshers_) f();
        return;
    }
    for (const auto& f : refreshers_) f();
}

void InspectorPanel::editing(bool began) {
    editing_ = std::max(0, editing_ + (began ? 1 : -1));
    if (!began && editing_ == 0 && pending_) {
        pending_ = false;
        QTimer::singleShot(0, this, &InspectorPanel::changed);
    }
}

void InspectorPanel::rebuild() {
    shown_ = signature();
    const int scrolled = scroll_->verticalScrollBar()->value();
    refreshers_.resize(1); // the header's
    body_ = new QWidget;
    auto* v = new QVBoxLayout(body_);
    v->setContentsMargins(16, 4, 16, 18);
    v->setSpacing(22);
    const auto clips = selected();
    if (clips.empty() && showsCaptions()) captionSection(v);
    else if (clips.empty()) sequenceSection(v);
    else clipSections(v, clips);
    v->addStretch();
    scroll_->setWidget(body_);
    scroll_->verticalScrollBar()->setValue(scrolled);
    for (const auto& f : refreshers_) f();
}

// MARK: Captions

/// The picked caption's words, to retype, and the style every caption shares.
void InspectorPanel::captionSection(QVBoxLayout* v) {
    EditorModel& m = model();
    if (!m.project().captions) return;
    std::vector<CaptionBlock> picked;
    for (const auto& b : m.captionBlocks()) if (m.captionSelection().count(b.id())) picked.push_back(b);
    if (picked.size() == 1) {
        auto* s = new InspectorSection(QStringLiteral("Caption"));
        auto* field = new CaptionTextField(m, picked.front());
        s->add(field);
        s->add(note(QStringLiteral("Fixed here, a word stays fixed wherever this clip is used. Delete leaves the caption's words out.")));
        const QString id = picked.front().id();
        onRefresh([this, field, id] {
            for (const auto& b : model().captionBlocks()) if (b.id() == id) { field->setBlock(b); return; }
        });
        v->addWidget(s);
    }
    auto* s = new InspectorSection(QStringLiteral("Style"));
    const CaptionStyle st = m.project().captions->style;
    auto* size = new ParamRow({RowIcon::symbol(QStringLiteral("textformat.size")), QStringLiteral("Size"), st.size, CaptionStyle::sizeMin,
                               CaptionStyle::sizeMax, false, std::nullopt, whole,
                               [this](bool b) { editing(b); if (!b) model().commitLive(); },
                               [this](double val) { model().setCaptionStyle([val](CaptionStyle& x) { x.size = std::round(val); }, true); }});
    auto* pos = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.up.and.down")), QStringLiteral("Position"), st.y * 100, 10, 95, false,
                              std::nullopt, [](double val) { return whole(val) + u'%'; },
                              [this](bool b) { editing(b); if (!b) model().commitLive(); },
                              [this](double val) { model().setCaptionStyle([val](CaptionStyle& x) { x.y = std::round(val / 100 * 1000) / 1000; }, true); }});
    s->add(size);
    s->add(pos);
    onRefresh([this, size, pos] {
        if (!model().project().captions) return;
        const CaptionStyle& x = model().project().captions->style;
        size->setValue(x.size);
        pos->setValue(x.y * 100);
    });
    s->add(captionColorRow(QStringLiteral("Text"), QStringLiteral("textformat"), std::nullopt, [](const CaptionStyle& x) { return x.textColor; },
                           nullptr, [](CaptionStyle& x, RGBA c) { x.textColor = c; }));
    s->add(captionColorRow(QStringLiteral("Word highlight"), QStringLiteral("highlighter"), st.highlight,
                           [](const CaptionStyle& x) { return x.highlightColor; }, [](CaptionStyle& x) { x.highlight = !x.highlight; },
                           [](CaptionStyle& x, RGBA c) { x.highlightColor = c; }));
    s->add(captionColorRow(QStringLiteral("Outline"), QStringLiteral("a.square"), st.outline, [](const CaptionStyle& x) { return x.outlineColor; },
                           [](CaptionStyle& x) { x.outline = !x.outline; }, [](CaptionStyle& x, RGBA c) { x.outlineColor = c; }));
    if (m.captionsWaiting()) {
        s->add(note(QStringLiteral("Listening for what's said in the captioned clips…")));
    } else if (const QString language = c_.transcripts().unsupportedLanguage(); !language.isEmpty()) {
        // Said, rather than captions made from the wrong language.
        auto* n = note(QStringLiteral("%1 speech can't be transcribed on this computer yet. You can choose the speech language in Settings › General.").arg(language));
        n->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::textDim().name()));
        s->add(n);
    }
    v->addWidget(s);
}

/// A caption colour: its well, and for a part that can be off (highlight, outline) an eye to turn it on.
QWidget* InspectorPanel::captionColorRow(const QString& label, const QString& icon, std::optional<bool> on,
                                         std::function<RGBA(const CaptionStyle&)> get, std::function<void(CaptionStyle&)> toggle,
                                         std::function<void(CaptionStyle&, RGBA)> set) {
    const CaptionStyle st = model().project().captions->style;
    auto* well = new ColorField(get(st), [this, set](RGBA c) { model().setCaptionStyle([&](CaptionStyle& x) { set(x, c); }, true, true); });
    well->setFixedWidth(44);
    std::vector<QWidget*> right;
    QWidget* row = nullptr;
    const bool off = on.has_value() && !*on;
    if (on && toggle) {
        auto* eye = new CaptionEye(*on, [this, toggle] { model().setCaptionStyle(toggle); });
        // An eye right after the name, to show or hide them (as Grade's beside its title).
        row = labelledRow(RowIcon::symbol(icon), label, {well}, off, -1, eye);
    } else {
        row = labelledRow(RowIcon::symbol(icon), label, {well}, off);
    }
    if (off) {
        auto* fx = new QGraphicsOpacityEffect(well);
        fx->setOpacity(0.4);
        well->setGraphicsEffect(fx);
    }
    onRefresh([this, well, get] { if (model().project().captions) well->setColor(get(model().project().captions->style)); });
    return row;
}

// MARK: Sequence

void InspectorPanel::sequenceSection(QVBoxLayout* v) {
    EditorModel& m = model();
    const Project& p = m.project();
    auto* frame = new InspectorSection(QStringLiteral("Frame"));
    auto* size = new MenuField(QStringLiteral("%1 × %2").arg(p.width).arg(p.height));
    for (const auto& [label, w, h] : kSizes) {
        const int ww = w, hh = h;
        size->menu()->addAction(label, this, [&m, ww, hh] { m.setSequence(ww, hh); });
    }
    frame->add(size);
    auto* fields = new FrameSizeFields(p.width, p.height, [&m](int w, int h) { m.setSequence(w, h); });
    frame->add(fields);
    v->addWidget(frame);
    auto* rate = new InspectorSection(QStringLiteral("Frame rate"));
    auto* fps = new MenuField(fpsLabel(p.fps));
    for (double r : EditorModel::frameRates()) fps->menu()->addAction(fpsLabel(r), this, [&m, r] { m.setFPS(r); });
    rate->add(fps);
    bool hasVideo = false;
    for (const auto& a : p.assets) if (a.kind == MediaKind::video) hasVideo = true;
    if (!p.settingsLocked && !hasVideo) rate->add(note(QStringLiteral("Matches the first video you import."), 10));
    v->addWidget(rate);
    // Folded unless opened (it stays as left): most work is SDR, and HDR is picked here on purpose. Folded, a dot says
    // the project is HDR.
    auto* range = new InspectorSection(QStringLiteral("Dynamic range"), std::nullopt, true, p.isHDR());
    auto* dyn = new MenuField(p.isHDR() ? QStringLiteral("HDR (HLG)") : QStringLiteral("SDR (Rec. 709)"));
    dyn->menu()->addAction(QStringLiteral("SDR (Rec. 709)"), this, [&m] { m.setHDR(false); });
    dyn->menu()->addAction(QStringLiteral("HDR (HLG)"), this, [&m] { m.setHDR(true); });
    range->add(dyn);
    range->add(note(p.isHDR() ? QStringLiteral("HDR footage keeps its bright highlights and exports as 10\u2011bit HEVC.")
                              : QStringLiteral("HDR footage is converted to SDR."), 10));
    v->addWidget(range);
    onRefresh([this, size, fields, fps] {
        const Project& q = model().project();
        size->setLabel(QStringLiteral("%1 × %2").arg(q.width).arg(q.height));
        fields->set(q.width, q.height);
        fps->setLabel(fpsLabel(q.fps));
    });
}

// MARK: Clips

void InspectorPanel::clipSections(QVBoxLayout* v, const std::vector<Clip>& clips) {
    EditorModel& m = model();
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    std::vector<Clip> visual, audio, sourced;
    for (const auto& c : clips) {
        if (c.kind != ClipKind::audio) visual.push_back(c); else audio.push_back(c);
        if (c.playsSource()) sourced.push_back(c);
    }
    const bool offSpeed = !sourced.empty() && std::abs(sourced.front().speed - 1) > 0.001;
    const bool offGain = !audio.empty() && std::abs(audio.front().gainDB) > 0.001;

    // What a title or a solid is comes first: its words, or its colour.
    if (clips.size() == 1) {
        const Clip& c = clips.front();
        if (c.kind == ClipKind::text) textSection(v, c);
        if (c.kind == ClipKind::solid) {
            // Named for the clip, like Text; Fill is its colour, as a row like the others.
            auto* s = new InspectorSection(QStringLiteral("Solid"));
            const Uuid id = c.id;
            auto* well = new ColorField(c.color, [&m, id](RGBA col) {
                m.live([&](Project& p) { if (auto i = p.index(id)) p.clips[*i].color = col; });
            });
            s->add(labelledRow(RowIcon::symbol(QStringLiteral("paintbrush")), QStringLiteral("Fill"), {well}));
            onRefresh([this, well, id] { if (const Clip* x = findClip(model().project(), id)) well->setColor(x->color); });
            v->addWidget(s);
        }
    }

    if (!sourced.empty() || !audio.empty()) {
        const bool ramped = !sourced.empty() && sourced.front().isRamped();
        IdSet audioIDs;
        for (const auto& a : audio) audioIDs.insert(a.id);
        std::optional<InspectorSection::Action> reset;
        // Like the other sections: Reset puts the whole section back (speed, ramp and volume).
        if (offSpeed || ramped || offGain) reset = InspectorSection::Action{QStringLiteral("Reset"), [&m, audioIDs] { m.resetPlayback(audioIDs); }};
        auto* s = new InspectorSection(QStringLiteral("Playback"), reset, false, offSpeed || ramped || offGain);
        if (!audio.empty()) {
            const Uuid first = audio.front().id;
            auto* vol = new ParamRow({RowIcon::symbol(QStringLiteral("speaker.wave.2")), QStringLiteral("Volume"), audio.front().gainDB, -60, 12,
                                      false, std::nullopt, [](double d) { return formatDB(d); },
                                      [this](bool b) { editing(b); if (!b) model().commitLive(); },
                                      [&m, audioIDs](double val) {
                                          m.live([&](Project& p) { for (auto& x : p.clips) if (audioIDs.count(x.id)) x.gainDB = std::round(val * 10) / 10; }, false);
                                      }});
            s->add(vol);
            onRefresh([this, vol, first] { if (const Clip* x = findClip(model().project(), first)) vol->setValue(x->gainDB); });
        }
        if (!sourced.empty()) {
            const Clip first = sourced.front();
            const Uuid fid = first.id;
            auto* ramp = new RampButton(ramped, ramped ? QStringLiteral("Remove the speed ramp") : QStringLiteral("Speed ramp: ease into and out of the speed"),
                                        [this, fid, ids] { if (const Clip* x = findClip(model().project(), fid)) toggleRamp(*x, ids); });
            auto* speed = new ParamRow({RowIcon::symbol(QStringLiteral("clock.arrow.circlepath")), QStringLiteral("Speed"), first.speed * 100, 10, 5000,
                                        true, std::nullopt, [](double d) { return whole(d) + u'%'; },
                                        [this](bool began) {
                                            editing(began);
                                            if (began) {
                                                speedOrigin_ = model().project().clips;
                                            } else {
                                                speedOrigin_.reset();
                                                model().commitLive();
                                            }
                                        },
                                        [this, ids](double d) { applySpeed(d / 100, ids); }, nullptr, ramp});
            s->add(speed);
            onRefresh([this, speed, fid] { if (const Clip* x = findClip(model().project(), fid)) speed->setValue(x->speed * 100); });
            // The ramp, while the clip has one (kept up mid-drag, even if a dot is pulled back out).
            if (ramped || rampOrigin_) {
                auto* graph = new SpeedRampGraph(first, m.project().fps);
                graph->onEditing = [this](bool began) {
                    model().setShowPlayheadTime(began);
                    editing(began);
                    if (began) {
                        rampOrigin_ = model().project().clips;
                    } else {
                        rampOrigin_.reset();
                        model().commitLive();
                        if (auto back = rampPlayhead_) {
                            rampPlayhead_.reset();
                            model().seek(*back);
                        }
                    }
                };
                graph->onChange = [this, ids, fid](const SpeedRampGraph::Shape& shape, std::optional<double> focus) {
                    if (!rampOrigin_) return;
                    const bool sticky = model().sticky();
                    const auto origin = *rampOrigin_;
                    model().live([&](Project& p) {
                        p.clips = origin;
                        p.setRamp(p.linked(ids), shape.mid, shape.p1, shape.p2, shape.p3, shape.p4, shape.start, shape.end, sticky);
                    }, false);
                    // The playhead and viewer follow the dot being dragged, to show the frame it's on (never while
                    // playing, which would stop playback).
                    if (focus && !model().isPlaying()) {
                        if (const Clip* c = findClip(model().project(), fid)) {
                            if (!rampPlayhead_) rampPlayhead_ = model().playhead();
                            model().scrub(SpeedRampGraph::time(*focus, *c));
                        }
                    }
                };
                graph->onSet = [this, ids](const SpeedRampGraph::Shape& shape) {
                    const bool sticky = model().sticky();
                    model().mutate([&](Project& p) {
                        p.setRamp(p.linked(ids), shape.mid, shape.p1, shape.p2, shape.p3, shape.p4, shape.start, shape.end, sticky);
                    });
                };
                s->add(graph);
                onRefresh([this, graph, fid] { if (const Clip* x = findClip(model().project(), fid)) graph->setClip(*x); });
            }
        }
        v->addWidget(s);
    }

    std::vector<Clip> movable;
    for (const auto& c : visual) if (c.isTransformable()) movable.push_back(c);
    if (!movable.empty()) transformSection(v, movable);
    else if (visual.size() == 1 && visual.front().kind == ClipKind::text) textTransformSection(v, visual.front());
    if (visual.size() == 1) transitionSection(v, visual.front());
    if (!visual.empty()) fadeSection(v, audio.empty() ? QStringLiteral("Fade") : QStringLiteral("Video fade"), visual);
    if (!audio.empty()) fadeSection(v, visual.empty() ? QStringLiteral("Fade") : QStringLiteral("Audio fade"), audio);
    if (!visual.empty()) colorSection(v, visual);
}

/// The ramp button: on, a starter ramp (100%, easing into the speed in the middle of the clip, back to 100%;
/// a dip to 50% if the speed is 100%); off, the ramp goes and the speed stays.
void InspectorPanel::toggleRamp(const Clip& clip, const IdSet& ids) {
    EditorModel& m = model();
    const IdSet group = m.project().linked(ids);
    const bool sticky = m.sticky();
    if (clip.isRamped()) {
        m.mutate([&](Project& p) { p.retime(group, sticky, [](Clip& c) { c.ramp.reset(); }); });
    } else {
        const double mid = std::abs(clip.speed - 1) < 0.001 ? 0.5 : clip.speed;
        // In the shape last picked on the graph: easing in and back out, or just in (2-point).
        const bool in = SpeedRampGraph::lastMode() == SpeedRampGraph::Mode::rampIn;
        const double p1 = 0.3, p2 = in ? 0.6 : 0.4, p3 = in ? 1.0 : 0.6, p4 = in ? 1.0 : 0.7;
        m.mutate([&](Project& p) { p.setRamp(group, mid, p1, p2, p3, p4, 1, 1, sticky); });
    }
}

void InspectorPanel::applySpeed(double speed, const IdSet& ids) {
    EditorModel& m = model();
    if (!speedOrigin_) {
        m.setSpeed(speed);
        return;
    }
    // Always derive from the clips as they were when the drag began, so rounding never accumulates.
    const auto origin = *speedOrigin_;
    const bool sticky = m.sticky();
    m.live([&](Project& p) {
        p.clips = origin;
        p.setSpeed(p.linked(ids), speed, sticky);
    }, false);
}

void InspectorPanel::transformSection(QVBoxLayout* v, const std::vector<Clip>& clips) {
    EditorModel& m = model();
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    const ClipTransform tf = clips.front().transform.value_or(ClipTransform());
    const double w = m.project().width, h = m.project().height;
    const Uuid fid = clips.front().id;
    const bool single = clips.size() == 1;
    auto live = [&m, ids](std::function<void(ClipTransform&)> body) {
        m.live([&](Project& p) {
            for (auto& x : p.clips) {
                if (!ids.count(x.id)) continue;
                ClipTransform t = x.transform.value_or(ClipTransform());
                body(t);
                if (t.isIdentity()) x.transform.reset(); else x.transform = t;
            }
        }, false);
    };
    auto done = [this](bool began) { editing(began); if (!began) model().commitLive(); };
    auto current = [this, fid] {
        const Clip* c = findClip(model().project(), fid);
        return c ? c->transform.value_or(ClipTransform()) : ClipTransform();
    };
    std::optional<InspectorSection::Action> reset;
    if (!tf.isIdentity())
        reset = InspectorSection::Action{QStringLiteral("Reset"), [&m, ids] {
            m.mutate([&](Project& p) { for (auto& x : p.clips) if (ids.count(x.id)) x.transform.reset(); });
        }};
    auto* s = new InspectorSection(QStringLiteral("Transform"), reset, true, !tf.isIdentity());
    QWidget* zoomButton = nullptr;
    if (single)
        zoomButton = new RampButton(tf.zoom.has_value(),
                                    tf.zoom ? QStringLiteral("Remove the zoom") : QStringLiteral("Zoom: move between the full frame and this scale over the clip"),
                                    [&m, fid] { m.toggleZoom(fid); });
    auto* scale = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.up.left.and.arrow.down.right")), QStringLiteral("Scale"), tf.scale * 100, 5, 1000,
                                true, 1.0, percentTenths,
                                [this, done, single, fid](bool began) {
                                    // Zooming: show the zoomed-in framing while its scale is set.
                                    if (began && single) model().showZoomed(fid);
                                    done(began);
                                },
                                [live](double val) {
                                    live([val](ClipTransform& t) {
                                        t.scale = std::clamp(std::round(val * 10) / 1000, ClipTransform::scaleMin, ClipTransform::scaleMax);
                                        if (t.fills()) std::tie(t.x, t.y) = t.zoomedOffset();
                                    });
                                },
                                nullptr, zoomButton});
    s->add(scale);
    onRefresh([scale, current] { scale->setValue(current().scale * 100); });
    if (single && tf.zoom) {
        const Clip c = clips.front();
        auto* graph = new ZoomGraph(c, *tf.zoom, tf.scale, m.project().fps);
        graph->onEditing = [this](bool began) {
            model().setShowPlayheadTime(began);
            editing(began);
            if (!began) {
                model().commitLive();
                if (auto back = zoomPlayhead_) { zoomPlayhead_.reset(); model().seek(*back); }
            }
        };
        graph->onChange = [this, live, fid](const ZoomMove& z, double sc, std::optional<double> focus) {
            live([&](ClipTransform& t) { t.zoom = z; t.scale = sc; std::tie(t.x, t.y) = t.zoomedOffset(); });
            // The playhead and viewer follow the dot being dragged (never while playing), showing the new curve
            // there, not the one from before the drag moved.
            if (focus && !model().isPlaying()) {
                model().flushRebuild();
                if (!zoomPlayhead_) zoomPlayhead_ = model().playhead();
                if (const Clip* x = findClip(model().project(), fid)) model().scrub(x->start + *focus);
            }
        };
        graph->onSet = [&m, fid](const ZoomMove& z) {
            QSettings().setValue(QLatin1String(ZoomMove::modeKey), z.mode == ZoomMove::Mode::zoomOut ? QStringLiteral("out")
                                                                   : z.mode == ZoomMove::Mode::inOut ? QStringLiteral("inOut") : QStringLiteral("in"));
            m.mutate([&](Project& p) { if (auto i = p.index(fid); i && p.clips[*i].transform) p.clips[*i].transform->zoom = z; });
        };
        s->add(graph);
        onRefresh([this, graph, fid] {
            const Clip* x = findClip(model().project(), fid);
            if (x && x->transform && x->transform->zoom) graph->set(*x, *x->transform->zoom, x->transform->scale);
        });
    }
    ParamRow* xr;
    ParamRow* yr;
    if (tf.fills()) {
        // Bigger than the frame (zoomed in, or zooming): X and Y say where it frames (right and down are positive),
        // only as far as the picture reaches. The picture itself moves the other way.
        const double mm = std::max(0.0, tf.scale - 1) / 2;
        const auto at = tf.zoomedOffset();
        auto began = [this, done, single, fid](bool b) { if (b && single) model().showZoomed(fid); done(b); };
        xr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.left.and.right")), QStringLiteral("X"), -at.first * w, -mm * w, std::max(mm * w, 0.001),
                           false, 1.0, tenths, began,
                           [live, w](double val) {
                               live([&](ClipTransform& t) {
                                   const double m2 = std::max(0.0, t.scale - 1) / 2;
                                   t.x = -std::clamp(std::round(val * 10) / 10 / w, -m2, m2);
                               });
                           }});
        yr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.up.and.down")), QStringLiteral("Y"), -at.second * h, -mm * h, std::max(mm * h, 0.001),
                           false, 1.0, tenths, began,
                           [live, h](double val) {
                               live([&](ClipTransform& t) {
                                   const double m2 = std::max(0.0, t.scale - 1) / 2;
                                   t.y = -std::clamp(std::round(val * 10) / 10 / h, -m2, m2);
                               });
                           }});
        onRefresh([xr, yr, current, w, h] {
            const auto a = current().zoomedOffset();
            xr->setValue(-a.first * w);
            yr->setValue(-a.second * h);
        });
    } else {
        // The frame's size or smaller: where the picture sits, as far as fully off the frame.
        const double reach = (1 + tf.scale) / 2;
        xr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.left.and.right")), QStringLiteral("X"), tf.x * w, -reach * w, reach * w, false, 1.0,
                           tenths, done, [live, w](double val) { live([&](ClipTransform& t) { t.x = std::round(val * 10) / 10 / w; }); }});
        yr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.up.and.down")), QStringLiteral("Y"), tf.y * h, -reach * h, reach * h, false, 1.0,
                           tenths, done, [live, h](double val) { live([&](ClipTransform& t) { t.y = std::round(val * 10) / 10 / h; }); }});
        onRefresh([xr, yr, current, w, h] {
            const ClipTransform t = current();
            xr->setValue(t.x * w);
            yr->setValue(t.y * h);
        });
    }
    s->add(xr);
    s->add(yr);
    v->addWidget(s);
}

/// Transitions on the cuts either side of a clip: In blends from the clip before, Out into the clip after.
void InspectorPanel::transitionSection(QVBoxLayout* v, const Clip& c) {
    const Project& p = model().project();
    const Clip* before = p.previousTouching(c);
    const Clip* after = p.nextTouching(c);
    if (!before && !after) return;
    const bool marked = (before && c.transitionIn) || (after && after->transitionIn);
    auto* s = new InspectorSection(QStringLiteral("Transitions"), std::nullopt, true, marked);
    if (before) transitionRows(s, QStringLiteral("In"), c.id, before->id);
    if (after) transitionRows(s, QStringLiteral("Out"), after->id, c.id);
    v->addWidget(s);
}

void InspectorPanel::transitionRows(InspectorSection* s, const QString& label, const Uuid& into, const Uuid& from) {
    EditorModel& m = model();
    const Clip* clip = findClip(m.project(), into);
    if (!clip) return;
    const std::optional<Transition> t = clip->transitionIn;
    auto* menu = new MenuField(t ? transitionKindLabel(t->kind) : QStringLiteral("None"), 26);
    menu->menu()->addAction(QStringLiteral("None"), this, [&m, into] { m.setTransition(into, std::nullopt); });
    menu->menu()->addSeparator();
    for (auto kind : {TransitionKind::dissolve, TransitionKind::dipToBlack, TransitionKind::wipe}) {
        menu->menu()->addAction(transitionKindLabel(kind), this, [&m, into, kind] {
            const Clip* x = findClip(m.project(), into);
            Transition n = x && x->transitionIn ? *x->transitionIn : Transition();
            n.kind = kind;
            m.setTransition(into, n);
        });
    }
    s->add(labelledRow(RowIcon::symbol(label == QStringLiteral("In") ? QStringLiteral("arrow.right.to.line") : QStringLiteral("arrow.right.to.line.compact")),
                       label, {menu}, false, 52));
    onRefresh([this, menu, into] {
        const Clip* x = findClip(model().project(), into);
        if (x) menu->setLabel(x->transitionIn ? transitionKindLabel(x->transitionIn->kind) : QStringLiteral("None"));
    });
    if (!t) return;
    auto* length = new ParamRow({RowIcon::symbol(QStringLiteral("timer")), QStringLiteral("Length"), t->duration, 0.1, 5, false, std::nullopt, seconds2,
                                 [this](bool b) { editing(b); if (!b) model().commitLive(); },
                                 [&m, into](double val) {
                                     m.live([&](Project& p) {
                                         if (auto i = p.index(into); i && p.clips[*i].transitionIn) p.clips[*i].transitionIn->duration = std::round(val * 100) / 100;
                                     }, false);
                                 }});
    s->add(length);
    auto* shortened = note(QString(), 10, 26);
    s->add(shortened);
    onRefresh([this, length, shortened, into, from] {
        const Project& q = model().project();
        const Clip* x = findClip(q, into);
        const Clip* a = findClip(q, from);
        if (!x || !x->transitionIn || !a) { shortened->hide(); return; }
        length->setValue(x->transitionIn->duration);
        const auto span = q.transitionSpan(*a, *x);
        const double actual = span ? span->end - span->start : 0;
        if (actual < x->transitionIn->duration - 0.01) {
            shortened->setText(actual > 0 ? QString::asprintf("Shortened to %.2fs: no more media beyond the cut.", actual)
                                          : QStringLiteral("Needs media beyond the cut to blend; try Dip to Black."));
            shortened->show();
        } else {
            shortened->hide();
        }
    });
}

void InspectorPanel::fadeSection(QVBoxLayout* v, const QString& title, const std::vector<Clip>& clips) {
    EditorModel& m = model();
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    const Clip& first = clips.front();
    const Uuid fid = first.id;
    const double maxFade = std::max(0.1, std::min(first.duration, 10.0));
    auto* s = new InspectorSection(title, std::nullopt, true, first.fadeIn > 0 || first.fadeOut > 0);
    auto* inCurve = new CurveMenu(first.inCurve(), true, first.fadeIn > 0, [&m, ids](FadeCurve curve) {
        m.mutate([&](Project& p) { for (auto& x : p.clips) if (ids.count(x.id)) x.fadeInCurve = curve; });
    });
    auto* outCurve = new CurveMenu(first.outCurve(), false, first.fadeOut > 0, [&m, ids](FadeCurve curve) {
        m.mutate([&](Project& p) { for (auto& x : p.clips) if (ids.count(x.id)) x.fadeOutCurve = curve; });
    });
    auto done = [this](bool b) { editing(b); if (!b) model().commitLive(); };
    auto* fin = new ParamRow({RowIcon::fadeIn(), QStringLiteral("Fade In"), first.fadeIn, 0, maxFade, false, std::nullopt, seconds2, done,
                              [&m, ids](double val) {
                                  m.live([&](Project& p) {
                                      for (auto& x : p.clips) if (ids.count(x.id)) x.fadeIn = std::min(p.snapFrame(val), x.duration - x.fadeOut);
                                  }, false);
                              }, inCurve});
    auto* fout = new ParamRow({RowIcon::fadeOut(), QStringLiteral("Fade Out"), first.fadeOut, 0, maxFade, false, std::nullopt, seconds2, done,
                               [&m, ids](double val) {
                                   m.live([&](Project& p) {
                                       for (auto& x : p.clips) if (ids.count(x.id)) x.fadeOut = std::min(p.snapFrame(val), x.duration - x.fadeIn);
                                   }, false);
                               }, outCurve});
    s->add(fin);
    s->add(fout);
    onRefresh([this, fin, fout, inCurve, outCurve, fid] {
        const Clip* x = findClip(model().project(), fid);
        if (!x) return;
        fin->setValue(x->fadeIn);
        fout->setValue(x->fadeOut);
        inCurve->set(x->inCurve(), x->fadeIn > 0);
        outCurve->set(x->outCurve(), x->fadeOut > 0);
    });
    v->addWidget(s);
}

void InspectorPanel::colorSection(QVBoxLayout* v, const std::vector<Clip>& clips) {
    EditorModel& m = model();
    IdSet ids;
    for (const auto& c : clips) ids.insert(c.id);
    const Uuid fid = clips.front().id;
    const ColorGrade grade = clips.front().grade.value_or(ColorGrade());
    auto apply = [ids](Project& p, const std::function<void(ColorGrade&)>& body) {
        for (auto& x : p.clips) {
            if (!ids.count(x.id)) continue;
            ColorGrade g = x.grade.value_or(ColorGrade());
            // Changing a grade that's off turns it on, so it isn't adjusted blind.
            g.off = false;
            body(g);
            if (g.isNeutral()) x.grade.reset(); else x.grade = g;
        }
    };
    auto live = [&m, apply](std::function<void(ColorGrade&)> body) { m.live([&](Project& p) { apply(p, body); }, false); };
    auto once = [&m, apply](std::function<void(ColorGrade&)> body) { m.mutate([&](Project& p) { apply(p, body); }); };
    auto done = [this](bool b) { editing(b); if (!b) model().commitLive(); };
    auto current = [this, fid] {
        const Clip* c = findClip(model().project(), fid);
        return c && c->grade ? *c->grade : ColorGrade();
    };
    std::optional<InspectorSection::Action> reset;
    if (!grade.isNeutral()) reset = InspectorSection::Action{QStringLiteral("Reset"), [once] { once([](ColorGrade& g) { g = ColorGrade(); }); }};
    auto* s = new InspectorSection(QStringLiteral("Grade"), reset, true, !grade.isNeutral());
    bool graded = false, off = false;
    for (const auto& c : clips) {
        if (c.grade && !c.grade->isNeutral()) graded = true;
        if (c.grade && c.grade->off) off = true;
    }
    // The eye: the grade off (kept, but not shown or exported) or on again, for every picked clip that has one.
    if (graded) {
        s->setAccessory(new GradeEye(off, [&m, ids, off] {
            m.mutate([&](Project& p) {
                for (auto& x : p.clips) {
                    if (!ids.count(x.id) || !x.grade || x.grade->isNeutral()) continue;
                    x.grade->off = !off;
                }
            });
        }));
    }

    auto* wheels = new QWidget;
    auto* wh = new QHBoxLayout(wheels);
    wh->setContentsMargins(0, 4, 0, 4);
    wh->setSpacing(0);
    struct W { const char* title; WheelPoint ColorGrade::*field; double ColorGrade::*level; };
    for (const W& wd : {W{"Shadows", &ColorGrade::shadows, &ColorGrade::shadowsLevel}, W{"Midtones", &ColorGrade::midtones, &ColorGrade::midtonesLevel},
                        W{"Highlights", &ColorGrade::highlights, &ColorGrade::highlightsLevel}}) {
        const auto field = wd.field;
        const auto level = wd.level;
        auto* wheel = new ColorWheel(QLatin1String(wd.title), grade.*field, grade.*level,
                                     [this, live, field](WheelPoint pt) {
                                         if (editing_ == 0) editing(true);
                                         live([&](ColorGrade& g) { g.*field = pt; });
                                     },
                                     [this, live, level](double val) {
                                         if (editing_ == 0) editing(true);
                                         live([&](ColorGrade& g) { g.*level = val; });
                                     },
                                     [this] { model().commitLive(); if (editing_ > 0) editing(false); });
        wh->addWidget(wheel, 1);
        onRefresh([wheel, current, field, level] { wheel->setPoint(current().*field); wheel->setLevel(current().*level); });
    }
    s->add(wheels);

    struct Row { const char* icon; const char* label; double lo, hi; std::function<QString(double)> format;
                 std::function<double(const ColorGrade&)> get; std::function<void(ColorGrade&, double)> set; };
    const std::vector<Row> rows = {
        {"sun.max", "Exposure", -2, 2, signed2, [](const ColorGrade& g) { return g.exposure; },
         [](ColorGrade& g, double val) { g.exposure = std::round(val * 100) / 100; }},
        {"circle.lefthalf.filled", "Contrast", -50, 50, whole, [](const ColorGrade& g) { return g.contrast; },
         [](ColorGrade& g, double val) { g.contrast = std::round(val); }},
        {"drop", "Saturate", -100, 100, whole, [](const ColorGrade& g) { return g.saturation; },
         [](ColorGrade& g, double val) { g.saturation = std::round(val); }},
        {"thermometer.medium", "Temp", -50, 50, whole, [](const ColorGrade& g) { return g.temperature; },
         [](ColorGrade& g, double val) { g.temperature = std::round(val); }},
        {"eyedropper", "Tint", -50, 50, whole, [](const ColorGrade& g) { return g.tint; }, [](ColorGrade& g, double val) { g.tint = std::round(val); }},
    };
    for (const Row& r : rows) {
        const auto set = r.set;
        const auto get = r.get;
        auto* row = new ParamRow({RowIcon::symbol(QLatin1String(r.icon)), QLatin1String(r.label), get(grade), r.lo, r.hi, false, std::nullopt, r.format,
                                  done, [live, set](double val) { live([&](ColorGrade& g) { set(g, val); }); }});
        s->add(row);
        onRefresh([row, current, get] { row->setValue(get(current())); });
    }

    auto* look = new MenuField(grade.lookLabel(), 26);
    look->menu()->addAction(QStringLiteral("None"), this, [once] { once([](ColorGrade& g) { g.look.reset(); }); });
    look->menu()->addSeparator();
    for (const auto& [name, recipe] : BuiltInLook::all()) {
        const QString n = name;
        look->menu()->addAction(n, this, [once, n] { once([&](ColorGrade& g) { g.look = n; }); });
    }
    look->menu()->addSeparator();
    look->menu()->addAction(QStringLiteral("Load .cube…"), this, [this, once] {
        if (auto path = chooseCube()) once([&](ColorGrade& g) { g.look = *path; });
    });
    s->add(labelledRow(RowIcon::symbol(QStringLiteral("camera.filters")), QStringLiteral("Look"), {look}, false, 52));
    onRefresh([look, current] { look->setLabel(current().lookLabel()); });
    if (grade.look) {
        auto* intensity = new ParamRow({RowIcon::symbol(QStringLiteral("circle.dotted")), QStringLiteral("Intensity"), grade.lookIntensity * 100, 0, 100,
                                        false, std::nullopt, [](double d) { return whole(d) + u'%'; }, done,
                                        [live](double val) { live([&](ColorGrade& g) { g.lookIntensity = std::round(val) / 100; }); }});
        s->add(intensity);
        onRefresh([intensity, current] { intensity->setValue(current().lookIntensity * 100); });
    }
    v->addWidget(s);
}

std::optional<QString> InspectorPanel::chooseCube() {
    QFileDialog dlg(window());
    dlg.setNameFilter(QStringLiteral("Cube LUT (*.cube)"));
    dlg.setFileMode(QFileDialog::ExistingFile);
    dlg.setLabelText(QFileDialog::Accept, QStringLiteral("Load"));
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty()) return std::nullopt;
    const QString path = dlg.selectedFiles().front();
    if (!GradeBaker::cubeFile(path)) {
        c_.setImportError(QStringLiteral("“%1” isn't a 3D .cube LUT this app can read.").arg(QFileInfo(path).fileName()));
        return std::nullopt;
    }
    return path;
}

void InspectorPanel::textSection(QVBoxLayout* v, const Clip& c) {
    EditorModel& m = model();
    const Uuid id = c.id;
    auto update = [&m, id](std::function<void(TextStyle&)> body) {
        m.live([&](Project& p) { if (auto i = p.index(id)) body(p.clips[*i].text); });
    };
    auto* s = new InspectorSection(QStringLiteral("Text"));
    auto* edit = new QPlainTextEdit(c.text.string);
    edit->setFont(Theme::font(11.5));
    edit->setFixedHeight(68);
    edit->setFrameShape(QFrame::NoFrame);
    edit->setStyleSheet(QStringLiteral("QPlainTextEdit { background: %1; color: %2; border-radius: 8px; padding: 4px; }")
                            .arg(Theme::field().name(), Theme::text().name()));
    connect(edit, &QPlainTextEdit::textChanged, this, [edit, update] {
        const QString t = edit->toPlainText();
        update([&](TextStyle& x) { x.string = t; });
    });
    s->add(edit);
    auto* font = new FontPicker(c.text.fontName, [update](const QString& family) { update([&](TextStyle& x) { x.fontName = family; }); });
    s->add(font);
    auto* chips = new QHBoxLayout;
    chips->setSpacing(8);
    chips->setContentsMargins(0, 0, 0, 0);
    auto* bold = new Chip(QStringLiteral("Bold"), c.text.bold, [update] { update([](TextStyle& x) { x.bold = !x.bold; }); });
    chips->addWidget(bold);
    auto* color = new ColorField(c.text.color, [update](RGBA col) { update([&](TextStyle& x) { x.color = col; }); });
    chips->addWidget(color, 1);
    s->add(chips);
    auto* size = new ParamRow({RowIcon::symbol(QStringLiteral("textformat.size")), QStringLiteral("Size"), c.text.size, 12, 600, true, std::nullopt, whole,
                               [this](bool b) { editing(b); if (!b) model().commitLive(); },
                               [update](double val) { update([&](TextStyle& x) { x.size = std::round(val); }); }});
    s->add(size);
    onRefresh([this, edit, font, bold, color, size, id] {
        const Clip* x = findClip(model().project(), id);
        if (!x) return;
        if (!edit->hasFocus() && edit->toPlainText() != x->text.string) {
            QSignalBlocker b(edit);
            edit->setPlainText(x->text.string);
        }
        font->setSelection(x->text.fontName);
        bold->setActive(x->text.bold);
        color->setColor(x->text.color);
        size->setValue(x->text.size);
    });
    v->addWidget(s);
}

/// Position for a text clip, in frame pixels from the centre like other clips. No scale: the font size does
/// that and keeps the text sharp.
void InspectorPanel::textTransformSection(QVBoxLayout* v, const Clip& c) {
    EditorModel& m = model();
    const double w = m.project().width, h = m.project().height;
    const Uuid id = c.id;
    const bool centred = std::abs(c.text.x - 0.5) < 1e-6 && std::abs(c.text.y - 0.5) < 1e-6;
    auto update = [&m, id](std::function<void(TextStyle&)> body) {
        m.live([&](Project& p) { if (auto i = p.index(id)) body(p.clips[*i].text); }, false);
    };
    auto done = [this](bool b) { editing(b); if (!b) model().commitLive(); };
    std::optional<InspectorSection::Action> reset;
    if (!centred)
        reset = InspectorSection::Action{QStringLiteral("Reset"), [&m, id] {
            m.mutate([&](Project& p) { if (auto i = p.index(id)) { p.clips[*i].text.x = 0.5; p.clips[*i].text.y = 0.5; } });
        }};
    auto* s = new InspectorSection(QStringLiteral("Transform"), reset, true, !centred);
    auto* xr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.left.and.right")), QStringLiteral("X"), (c.text.x - 0.5) * w, -w / 2, w / 2, false,
                             std::nullopt, whole, done, [update, w](double val) { update([&](TextStyle& x) { x.x = 0.5 + std::round(val) / w; }); }});
    auto* yr = new ParamRow({RowIcon::symbol(QStringLiteral("arrow.up.and.down")), QStringLiteral("Y"), (c.text.y - 0.5) * h, -h / 2, h / 2, false,
                             std::nullopt, whole, done, [update, h](double val) { update([&](TextStyle& x) { x.y = 0.5 + std::round(val) / h; }); }});
    s->add(xr);
    s->add(yr);
    onRefresh([this, xr, yr, id, w, h] {
        if (const Clip* x = findClip(model().project(), id)) {
            xr->setValue((x->text.x - 0.5) * w);
            yr->setValue((x->text.y - 0.5) * h);
        }
    });
    v->addWidget(s);
}

} // namespace er
