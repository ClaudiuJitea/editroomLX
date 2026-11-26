#include "LibraryPanel.h"
#include "Symbols.h"
#include "Theme.h"
#include "Widgets.h"
#include "model/MediaFolders.h"
#include "model/Timecode.h"
#include <QAbstractButton>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QDrag>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollArea>
#include <QLinearGradient>
#include <QTimer>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QSettings>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

namespace er {

MediaLayout mediaLayout() {
    return QSettings().value(QStringLiteral("mediaLayout")).toString() == QStringLiteral("list") ? MediaLayout::list : MediaLayout::thumbnails;
}

void setMediaLayout(MediaLayout l) {
    QSettings().setValue(QStringLiteral("mediaLayout"), l == MediaLayout::list ? QStringLiteral("list") : QStringLiteral("thumbnails"));
}

namespace {

const char* kAssetMime = "application/x-editroom-asset";
QColor alpha(QColor c, double a) { c.setAlphaF(float(c.alphaF() * a)); return c; }
QColor yellow() { return QColor(0xFF, 0xD6, 0x0A); }

/// Words typed in "quotes": what's said.
QStringList phrasesIn(const QString& search) {
    static const QRegularExpression re(QStringLiteral("[\"“”]([^\"“”]*)[\"“”]?"));
    QStringList out;
    auto it = re.globalMatch(search.trimmed());
    while (it.hasNext()) {
        const QString p = it.next().captured(1).trimmed();
        if (!p.isEmpty()) out << p;
    }
    return out;
}

/// A button drawn by a function.
class Painted : public QAbstractButton {
public:
    using Paint = std::function<void(QPainter&, const QRectF&, bool hovering)>;
    Painted(QSize size, Paint paint, QWidget* parent = nullptr) : QAbstractButton(parent), paint_(std::move(paint)) { setFixedSize(size); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paint_(p, QRectF(rect()), underMouse());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    Paint paint_;
};

/// The Text and Solid buttons: click to add at the playhead, or drag to the timeline.
class AddButton : public Painted {
public:
    AddButton(Controller& c, const QString& symbol, DragPayload payload, const QString& help, QWidget* parent)
        : Painted(QSize(26, 26), [symbol](QPainter& p, const QRectF& r, bool hovering) {
              if (hovering) { p.setPen(Qt::NoPen); p.setBrush(Theme::hover()); p.drawRoundedRect(r, 7, 7); }
              Symbols::draw(p, symbol, r.center(), 12, hovering ? Theme::text() : Theme::textDim(), 500);
          }, parent), c_(c), payload_(payload) {
        setToolTip(help);
    }
protected:
    void mousePressEvent(QMouseEvent* e) override { press_ = e->position(); dragged_ = false; }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton) || (e->position() - press_).manhattanLength() < 6 || dragged_) return;
        dragged_ = true;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(QLatin1String(kAssetMime), (payload_.kind == DragPayload::Kind::text ? QByteArray("editroom:text") : QByteArray("editroom:solid")));
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (!dragged_) c_.model().addAtPlayhead(payload_); }
private:
    Controller& c_;
    DragPayload payload_;
    QPointF press_;
    bool dragged_ = false;
};

/// The search field: an icon (a speech bubble once a quote is typed), the text, a clear button; Esc clears.
class SearchField : public QLineEdit {
public:
    explicit SearchField(QWidget* parent) : QLineEdit(parent) {
        setFixedHeight(26);
        setFrame(false);
        setFont(Theme::font(11.5));
        setTextMargins(22, 0, 20, 0);
        setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border-radius: 7px; border: 1px solid transparent; padding: 0 4px; }"
                                     "QLineEdit:focus { border-color: %3; }")
                          .arg(Theme::field().name(), Theme::text().name(), alpha(Theme::text(), 0.85).name(QColor::HexArgb)));
        connect(this, &QLineEdit::textChanged, this, [this] {
            const bool saying = text().contains(u'"') || text().contains(QChar(0x201C));
            setToolTip(saying ? QStringLiteral("Searching what's said in your clips") : QString());
            update();
        });
    }
protected:
    void paintEvent(QPaintEvent* e) override {
        QLineEdit::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool saying = text().contains(u'"') || text().contains(QChar(0x201C));
        Symbols::draw(p, saying ? QStringLiteral("text.bubble") : QStringLiteral("magnifyingglass"), QPointF(9 + 6, height() / 2.0), 10.5,
                      saying ? Theme::text() : Theme::textFaint(), 500);
        if (!text().isEmpty()) Symbols::draw(p, QStringLiteral("xmark.circle.fill"), clearCentre(), 11, Theme::textFaint(), 500);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (!text().isEmpty() && QLineF(e->position(), clearCentre()).length() < 9) { clear(); return; }
        QLineEdit::mousePressEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape) { clear(); clearFocus(); return; }
        QLineEdit::keyPressEvent(e);
    }
private:
    QPointF clearCentre() const { return QPointF(width() - 9 - 6, height() / 2.0); }
};

QString detailOf(const MediaAsset& a) {
    switch (a.kind) {
    case MediaKind::video: return Timecode::shortString(a.duration);
    case MediaKind::audio: return Timecode::shortString(a.duration) + QStringLiteral("  ·  Audio");
    case MediaKind::image: return QStringLiteral("Photo");
    }
    return {};
}

/// On hover: everything about the file.
QString detailsOf(const MediaAsset& a) {
    QStringList parts{a.name};
    if (a.kind != MediaKind::image) parts << Timecode::shortString(a.duration);
    if (a.width > 0) parts << QStringLiteral("%1×%2").arg(a.width).arg(a.height);
    if (a.kind == MediaKind::video && a.fps > 0) parts << QStringLiteral("%1 fps").arg(std::round(a.fps * 100) / 100, 0, 'g', 6);
    if (a.kind == MediaKind::audio) parts << QStringLiteral("Audio");
    return parts.join(QStringLiteral(" · "));
}

} // namespace

// MARK: Items

MediaGrid::MediaGrid(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    setMouseTracking(true);
    connect(&c_.model(), &EditorModel::changed, this, &MediaGrid::relayout);
    connect(&c_.model(), &EditorModel::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(&c_.model(), &EditorModel::mediaSelectionChanged, this, &MediaGrid::relayout);
    connect(&c_, &Controller::importChanged, this, &MediaGrid::relayout);
    connect(&c_.thumbnails(), &Thumbnails::ready, this, qOverload<>(&QWidget::update));
    connect(&c_.media(), &MediaStore::visualChanged, this, qOverload<>(&QWidget::update));
}

void MediaGrid::setSearch(const QString& s) {
    search_ = s;
    relayout();
}

/// 64 points wide at the usual width, up to 104 as the panel is widened.
double MediaGrid::thumbWidth() const { return std::min(104.0, std::max(64.0, 64 + (width() + 24 - 252) * 0.21)); }

/// The search: words in "quotes" are what's said in a clip; anything else is in its name.
std::vector<MediaAsset> MediaGrid::shown(const std::vector<MediaAsset>& assets) const {
    const QString q = search_.trimmed();
    if (q.isEmpty()) return assets;
    static const QRegularExpression re(QStringLiteral("[\"“”]([^\"“”]*)[\"“”]?"));
    const QStringList phrases = phrasesIn(q);
    const QString rest = QString(q).remove(re).trimmed();
    std::vector<MediaAsset> out;
    for (const auto& a : assets) {
        // Plain words: in its name, or what it shows ("pool"), each word as it's typed.
        if (!rest.isEmpty() && !a.name.contains(rest, Qt::CaseInsensitive)) {
            const auto tags = c_.sceneTags(a.id);
            const QStringList words = rest.toLower().split(u' ', Qt::SkipEmptyParts);
            bool all = true;
            for (int i = 0; i < words.size(); ++i)
                if (!a.name.toLower().contains(words[i]) && !(tags && tags->shows(words[i], i == words.size() - 1))) all = false;
            if (!all) continue;
        }
        if (!phrases.isEmpty()) {
            // What's said: only clips with a transcript can match.
            auto t = c_.media().transcript(a.id);
            if (!t) continue;
            bool all = true;
            for (const QString& ph : phrases) if (!t->says(ph, true)) all = false;
            if (!all) continue;
        }
        out.push_back(a);
    }
    return out;
}

void MediaGrid::relayout() {
    if (layingOut_) return;
    layingOut_ = true;
    const EditorModel& m = c_.model();
    const Project& p = m.project();
    const MediaTree tree(p);
    std::optional<QString> folder = m.mediaFolder && tree.exists(*m.mediaFolder) ? m.mediaFolder : std::nullopt;
    const bool searching = !search_.trimmed().isEmpty();
    const auto assets = shown(searching ? (folder ? tree.assetsUnder(*folder, p.assets) : p.assets) : tree.assetsIn(folder, p.assets));
    const QStringList folders = searching ? QStringList() : tree.folders(folder);
    items_.clear();
    for (const QString& f : folders) { Item it; it.folder = true; it.folderPath = f; items_.push_back(it); }
    for (const auto& a : assets) {
        Item it;
        it.asset = a;
        // Where an item found by searching is, when that's not the folder open.
        if (searching) if (auto at = tree.folderOf(a); at && at != folder) it.place = tree.label(*at);
        items_.push_back(it);
    }
    const double W = width();
    double y = 0;
    if (mediaLayout() == MediaLayout::thumbnails) {
        // LazyVGrid(.adaptive(minimum: 88), spacing 8), rows 10 apart.
        const int columns = std::max(1, int((W + 8) / (88 + 8)));
        const double tw = (W - 8.0 * (columns - 1)) / columns;
        const double pictureH = (tw - 6) * 9 / 16;
        size_t i = 0;
        while (i < items_.size()) {
            double rowH = 0;
            for (int c = 0; c < columns && i < items_.size(); ++c, ++i) {
                Item& it = items_[i];
                const double h = 3 + pictureH + 5 + 14 + (it.place ? 12 : 0) + 3;
                it.rect = QRectF(c * (tw + 8), y, tw, h);
                it.picture = QRectF(it.rect.left() + 3, y + 3, tw - 6, pictureH);
                rowH = std::max(rowH, h);
            }
            y += rowH + 10;
        }
        if (!items_.empty()) y -= 10;
    } else {
        const double th = thumbWidth(), ph = std::round(th * 9 / 16);
        for (auto& it : items_) {
            const double h = 4 + std::max(ph, 30.0 + (it.place ? 13 : 0)) + 4;
            it.rect = QRectF(0, y, W, h);
            it.picture = QRectF(4, y + 4, th, ph);
            y += h + 3;
        }
        if (!items_.empty()) y -= 3;
    }
    setFixedHeight(int(std::ceil(std::max(0.0, y))) + 1);
    hover_.reset();
    layingOut_ = false;
    update();
    emit contentChanged();
}

void MediaGrid::resizeEvent(QResizeEvent* e) {
    if (e->oldSize().width() != width()) relayout();
}

std::optional<size_t> MediaGrid::itemAt(QPointF p) const {
    for (size_t i = 0; i < items_.size(); ++i) if (items_[i].rect.contains(p)) return i;
    return std::nullopt;
}

void MediaGrid::drawPicture(QPainter& p, const Item& it, bool compact) {
    const QRectF r = it.picture;
    const double radius = compact ? 5 : 6;
    QPainterPath clip;
    clip.addRoundedRect(r, radius, radius);
    p.save();
    p.setClipPath(clip);
    if (it.folder) {
        // A folder: its picture, and a warning when it's missing.
        const bool missing = !MediaFolders::isFolder(it.folderPath);
        // Quiet: a faint fill and no outline, so it sits back among the clips' (mostly dark) pictures.
        p.fillRect(r, Theme::ink(0.02));
        QColor sym = Theme::textDim();
        sym.setAlphaF(sym.alphaF() * 0.8);
        Symbols::draw(p, QStringLiteral("folder"), r.center(), compact ? 14 : 21, sym, 300);
        if (missing) {
            p.fillRect(r, QColor(0, 0, 0, 115));
            Symbols::draw(p, QStringLiteral("exclamationmark.triangle.fill"), r.center(), 13, yellow(), 500);
        }
        p.restore();
        return;
    }
    const MediaAsset& a = it.asset;
    p.fillRect(r, QColor(0, 0, 0, 128));
    const bool offline = c_.media().looksOffline(a);
    const bool scrubbing = hover_ && &items_[*hover_] == &it && scrub_;
    QImage img;
    if (scrubbing && a.kind == MediaKind::video) {
        // Quantised so nearby positions share frames; until one is made, the poster stands in.
        const double steps = 60;
        const double t = std::max(0.0, std::min(a.duration - 0.05, std::round(*scrub_ * steps) / steps * a.duration));
        img = c_.thumbnails().get(a, t, 240);
        if (img.isNull()) img = c_.thumbnails().cached(a, t, 240);
    }
    if (img.isNull() && a.kind != MediaKind::audio) img = a.kind == MediaKind::image ? c_.thumbnails().get(a, 0, 240) : c_.thumbnails().poster(a);
    if (!img.isNull()) {
        const double s = std::max(r.width() / img.width(), r.height() / img.height());
        p.drawImage(QRectF(r.center().x() - img.width() * s / 2, r.center().y() - img.height() * s / 2, img.width() * s, img.height() * s), img);
    } else {
        const QString sym = a.kind == MediaKind::audio ? QStringLiteral("waveform") : a.kind == MediaKind::image ? QStringLiteral("photo") : QStringLiteral("film");
        Symbols::draw(p, sym, r.center(), compact ? 13 : 20, a.kind == MediaKind::audio ? Theme::textDim() : Theme::textFaint(), 400);
    }
    if (offline) {
        p.fillRect(r, QColor(0, 0, 0, 153));
        Symbols::draw(p, QStringLiteral("exclamationmark.triangle.fill"), r.center(), 13, yellow(), 500);
    }
    if (scrubbing) p.fillRect(QRectF(r.left() + *scrub_ * r.width() - 0.75, r.top(), 1.5, r.height()), Qt::white);
    // The length, bottom right (Thumbnails).
    if (!compact && a.kind != MediaKind::image) {
        const QString t = Timecode::shortString(a.duration);
        const QFont f = Theme::mono(9.5, 500);
        const double tw = QFontMetricsF(f).horizontalAdvance(t);
        const QRectF badge(r.right() - 5 - tw - 8, r.bottom() - 5 - 15, tw + 8, 15);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawRoundedRect(badge, 3, 3);
        p.setFont(f);
        p.setPen(QColor(0xF2, 0xF2, 0xF0));
        p.drawText(badge, Qt::AlignCenter, t);
    }
    p.restore();
}

void MediaGrid::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const EditorModel& m = c_.model();
    const MediaTree tree(m.project());
    const bool thumbs = mediaLayout() == MediaLayout::thumbnails;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (!it.rect.intersects(QRectF(e->rect()))) continue;
        const bool hovering = hover_ == i;
        const bool picked = it.folder ? m.folderSelection.count(it.folderPath) > 0 : m.mediaSelection.count(it.asset.id) > 0;
        // Media no longer brightens the source of the selected timeline clip: Reveal in Media shows it instead.
        const bool selected = false;
        const double radius = thumbs ? 8 : 7;
        p.setPen(Qt::NoPen);
        if (picked) { p.setBrush(Theme::ink(0.12)); p.drawRoundedRect(it.rect, radius, radius); }
        else if (hovering) { p.setBrush(Theme::hover()); p.drawRoundedRect(it.rect, radius, radius); }
        drawPicture(p, it, !thumbs);
        const QColor nameColor = hovering || selected || picked ? Theme::text() : Theme::textDim();
        if (thumbs) {
            // The picture's outline: picked, or used by the timeline's selection.
            if (picked || selected) {
                p.setPen(QPen(picked ? Theme::ink(0.85) : Theme::control(), picked ? 1.5 : 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(it.picture.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            }
            const QRectF nameR(it.rect.left() + 5, it.picture.bottom() + 5, it.rect.width() - 10, 14);
            p.setFont(Theme::font(11, 500));
            p.setPen(nameColor);
            if (it.folder) {
                // Its name, and how many items are in it at the right.
                QString count;
                if (MediaFolders::isFolder(it.folderPath)) count = QString::number(tree.count(it.folderPath));
                const QFont mono = Theme::mono(10, 500);
                const double cw = count.isEmpty() ? 0 : QFontMetricsF(mono).horizontalAdvance(count) + 6;
                p.drawText(nameR.adjusted(0, 0, -cw, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           QFontMetrics(p.font()).elidedText(MediaTree::name(it.folderPath), Qt::ElideMiddle, int(nameR.width() - cw)));
                p.setFont(mono);
                p.setPen(Theme::textFaint());
                p.drawText(nameR, Qt::AlignVCenter | Qt::AlignRight, count);
            } else {
                p.drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(it.asset.name, Qt::ElideMiddle, int(nameR.width())));
                if (it.place) {
                    p.setFont(Theme::font(10.5));
                    p.setPen(Theme::textFaint());
                    p.drawText(QRectF(nameR.left(), nameR.bottom() - 2, nameR.width(), 13), Qt::AlignVCenter | Qt::AlignLeft,
                               QFontMetrics(p.font()).elidedText(*it.place, Qt::ElideLeft, int(nameR.width())));
                }
            }
        } else {
            if (picked || selected) {
                p.setPen(QPen(picked ? Theme::ink(0.18) : Theme::control(), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(it.rect.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
            }
            const double tx = it.picture.right() + 9;
            const double top = it.rect.center().y() - (it.place ? 21 : 14);
            const QRectF nameR(tx, top, it.rect.right() - tx - 4, 15);
            p.setFont(Theme::font(11.5, 500));
            p.setPen(nameColor);
            const QString name = it.folder ? MediaTree::name(it.folderPath) : it.asset.name;
            p.drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(name, Qt::ElideMiddle, int(nameR.width())));
            QString detail;
            if (it.folder) {
                const int n = tree.count(it.folderPath);
                detail = MediaFolders::isFolder(it.folderPath) ? QStringLiteral("%1 item%2").arg(n).arg(n == 1 ? "" : "s") : QStringLiteral("Missing");
            } else {
                detail = detailOf(it.asset);
            }
            p.setFont(Theme::mono(10, 500));
            p.setPen(Theme::textFaint());
            p.drawText(QRectF(tx, nameR.bottom() + 2, nameR.width(), 13), Qt::AlignVCenter | Qt::AlignLeft, detail);
            if (it.place) {
                p.setFont(Theme::font(10.5));
                p.drawText(QRectF(tx, nameR.bottom() + 16, nameR.width(), 13), Qt::AlignVCenter | Qt::AlignLeft,
                           QFontMetrics(p.font()).elidedText(*it.place, Qt::ElideLeft, int(nameR.width())));
            }
        }
    }
}

void MediaGrid::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if ((e->buttons() & Qt::LeftButton) && pressed_ && (pos - press_).manhattanLength() >= 6) {
        const Item it = items_[*pressed_];
        pressed_.reset();
        if (it.folder) return;
        // Dragging one of several picked items takes them all, in the Media panel's order.
        const EditorModel& m = c_.model();
        QStringList ids;
        if (m.mediaSelection.count(it.asset.id) && m.mediaSelection.size() + m.folderSelection.size() > 1) {
            for (const auto& a : m.project().assets) if (m.mediaSelection.count(a.id)) ids << a.id.toString();
        } else {
            ids << it.asset.id.toString();
        }
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(QLatin1String(kAssetMime), (QStringLiteral("editroom:") + ids.join(u',')).toUtf8());
        drag->setMimeData(mime);
        c_.sourcePreview().clear();
        drag->exec(Qt::CopyAction);
        return;
    }
    const auto at = itemAt(pos);
    std::optional<double> scrub;
    if (at && !items_[*at].folder) {
        const Item& it = items_[*at];
        const MediaAsset& a = it.asset;
        // Moving across a video's picture scrubs through it; the main viewer shows it too, full size and in
        // playback's colours.
        if (a.kind == MediaKind::video && !c_.media().looksOffline(a) && a.duration > 0 && it.picture.contains(pos)) {
            scrub = std::clamp((pos.x() - it.picture.left()) / std::max(1.0, it.picture.width()), 0.0, 1.0);
            c_.sourcePreview().show(a, std::max(0.0, std::min(a.duration - 0.02, *scrub * a.duration)));
        }
        setToolTip(mediaLayout() == MediaLayout::list ? detailsOf(a) : QString());
    } else if (at) {
        const QString f = items_[*at].folderPath;
        const MediaTree tree(c_.model().project());
        const int n = tree.count(f);
        setToolTip(!MediaFolders::isFolder(f) ? QStringLiteral("%1 can't be found. Right-click to locate it.").arg(MediaTree::name(f))
                                              : QStringLiteral("%1 · %2 item%3. Double-click to open; it follows the folder on your computer.")
                                                    .arg(MediaTree::name(f)).arg(n).arg(n == 1 ? "" : "s"));
    } else {
        setToolTip(QString());
    }
    if (scrub_ && !scrub) c_.sourcePreview().clear();
    if (at != hover_ || scrub != scrub_) {
        hover_ = at;
        scrub_ = scrub;
        update();
    }
}

void MediaGrid::leaveEvent(QEvent*) {
    if (scrub_) c_.sourcePreview().clear();
    hover_.reset();
    scrub_.reset();
    update();
}

namespace {
/// A click on empty space (between items or below them) lets go of what's picked in Media; the timeline's
/// selection is left as it is.
void clearPicks(EditorModel& m) {
    if (m.mediaSelection.empty() && m.folderSelection.empty()) return;
    m.mediaSelection.clear();
    m.folderSelection.clear();
    emit m.mediaSelectionChanged();
}
} // namespace

void MediaGrid::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    press_ = e->position();
    pressed_ = itemAt(e->position());
    if (!pressed_) { clearPicks(c_.model()); return; }
    // A click picks it (Shift for a range, Ctrl to add or take away).
    std::vector<MediaPick> order;
    for (const auto& it : items_) order.push_back(it.pick());
    c_.model().clickMedia(items_[*pressed_].pick(), order, e->modifiers());
    // Out of the search field, so Delete reaches the editor rather than the text.
    setFocus();
}

void MediaGrid::mouseReleaseEvent(QMouseEvent*) { pressed_.reset(); }

void MediaGrid::mouseDoubleClickEvent(QMouseEvent* e) {
    auto i = itemAt(e->position());
    if (!i) return;
    const Item it = items_[*i];
    if (it.folder) openFolder(it.folderPath);
    else c_.model().addAtPlayhead(DragPayload::ofAsset(it.asset.id));
}

void MediaGrid::openFolder(const QString& folder) {
    // Opening a folder clears the search.
    emit folderOpened();
    c_.model().openMediaFolder(folder);
}

void MediaGrid::contextMenuEvent(QContextMenuEvent* e) {
    auto i = itemAt(e->pos());
    // On empty space, as in Finder: what's picked is let go first.
    if (!i) { clearPicks(c_.model()); LibraryPanel::panelMenu(c_, this, e->globalPos()); return; }
    const Item it = items_[*i];
    if (it.folder) folderMenu(it, e->globalPos()); else assetMenu(it, e->globalPos());
}

void MediaGrid::assetMenu(const Item& it, QPoint at) {
    EditorModel& m = c_.model();
    const MediaAsset a = it.asset;
    const bool offline = c_.media().looksOffline(a);
    const bool picked = m.mediaSelection.count(a.id) > 0;
    const size_t pickedCount = m.mediaSelection.size() + m.folderSelection.size();
    QMenu menu(this);
    if (offline) {
        menu.addAction(QStringLiteral("Locate…"), this, [this, a] { c_.locate(a.id, false); });
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Add to Timeline"), this, [&m, a] { m.addAtPlayhead(DragPayload::ofAsset(a.id)); });
    if (a.kind == MediaKind::video) menu.addAction(QStringLiteral("Match Sequence to Clip"), this, [&m, a] { m.matchSequence(a); })->setEnabled(!m.sequenceMatches(a));
    menu.addAction(QStringLiteral("Show in File Manager"), this, [a] { Controller::showInFileManager(a.path); });
    if (!offline) {
        // Normally automatic (when Editroom comes back to the front); this is for being sure.
        const IdSet ids = picked && pickedCount > 1 ? m.mediaSelection : IdSet{a.id};
        menu.addAction(ids.size() > 1 ? QStringLiteral("Reload %1 Items from Disk").arg(ids.size()) : QStringLiteral("Reload from Disk"), this,
                       [this, ids] { c_.reloadMedia(ids); });
    }
    menu.addSeparator();
    // The picked items when this is one of them; otherwise just this one.
    if (picked && pickedCount > 1) menu.addAction(QStringLiteral("Delete %1 Items").arg(pickedCount), this, [&m] { m.deleteSelected(); });
    else menu.addAction(QStringLiteral("Delete"), this, [&m, a] { m.removeAssets({a.id}); });
    menu.exec(at);
}

void MediaGrid::folderMenu(const Item& it, QPoint at) {
    EditorModel& m = c_.model();
    const QString folder = it.folderPath;
    const MediaTree tree(m.project());
    const MediaFolder* root = nullptr;
    for (const auto& r : tree.roots) if (r.path == folder) root = &r;
    const bool missing = !MediaFolders::isFolder(folder);
    const bool picked = m.folderSelection.count(folder) > 0;
    const size_t pickedCount = m.mediaSelection.size() + m.folderSelection.size();
    QMenu menu(this);
    if (missing && root) {
        const Uuid id = root->id;
        menu.addAction(QStringLiteral("Locate…"), this, [this, id] { c_.locateFolder(id); });
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Open"), this, [this, folder] { openFolder(folder); });
    if (!missing) {
        menu.addAction(QStringLiteral("Reload Folder from Disk"), this, [this, folder] { c_.reloadFolder(folder); });
        menu.addAction(QStringLiteral("Show in File Manager"), this, [folder] { Controller::showInFileManager(folder); });
    }
    menu.addSeparator();
    if (picked && pickedCount > 1) menu.addAction(QStringLiteral("Delete %1 Items").arg(pickedCount), this, [&m] { m.deleteSelected(); });
    else menu.addAction(QStringLiteral("Delete Folder"), this, [&m, folder] { m.removeFolder(folder); });
    menu.exec(at);
}

// MARK: Panel

/// The title: "MEDIA", or inside a folder, back (up one folder) and the open folder's name.
class MediaTitle : public QWidget {
public:
    MediaTitle(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
        setFixedHeight(20);
        setMouseTracking(true);
    }
    void refresh() {
        const auto folder = openFolder();
        const QFont f = Theme::label();
        QString t = folder ? MediaTree::name(*folder).toUpper() : QStringLiteral("MEDIA");
        setFixedWidth(int(std::ceil((folder ? 20 - 6 + 5 : 0) + QFontMetricsF(f).horizontalAdvance(t) + t.size() * 1.6 + 2)));
        setToolTip(folder ? MediaTree(c_.model().project()).label(*folder) : QString());
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto folder = openFolder();
        QFont f = Theme::label();
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
        p.setFont(f);
        double x = 0;
        if (folder) {
            const QRectF back(-6 + 6, 0, 20, 20);
            if (hoverBack_) { p.setPen(Qt::NoPen); p.setBrush(Theme::hover()); p.drawRoundedRect(back, 6, 6); }
            Symbols::draw(p, QStringLiteral("chevron.left"), back.center(), 10, hoverBack_ ? Theme::text() : Theme::textDim(), 600);
            x = back.right() + 5;
        }
        p.setPen(Theme::textDim());
        p.drawText(QRectF(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   folder ? MediaTree::name(*folder).toUpper() : QStringLiteral("MEDIA"));
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const bool h = openFolder() && e->position().x() < 20;
        if (h != hoverBack_) { hoverBack_ = h; update(); }
        if (openFolder() && h) {
            const MediaTree tree(c_.model().project());
            const QStringList trail = tree.trail(*openFolder());
            setToolTip(trail.size() > 1 ? QStringLiteral("Back to %1").arg(MediaTree::name(trail[trail.size() - 2])) : QStringLiteral("Back to Media"));
        }
    }
    void leaveEvent(QEvent*) override { hoverBack_ = false; update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (!openFolder() || e->position().x() >= 20) return;
        const MediaTree tree(c_.model().project());
        const QStringList trail = tree.trail(*openFolder());
        c_.model().openMediaFolder(trail.size() > 1 ? std::optional<QString>(trail[trail.size() - 2]) : std::nullopt);
    }
private:
    std::optional<QString> openFolder() const {
        const auto& f = c_.model().mediaFolder;
        return f && MediaTree(c_.model().project()).exists(*f) ? f : std::nullopt;
    }
    Controller& c_;
    bool hoverBack_ = false;
};

/// A small spinning ring while files come in.
class Spinner : public QWidget {
public:
    explicit Spinner(QWidget* parent) : QWidget(parent) {
        setFixedSize(14, 14);
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this] { angle_ = (angle_ + 30) % 360; update(); });
    }
    void setRunning(bool on) { setVisible(on); if (on) timer_.start(); else timer_.stop(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Theme::textDim(), 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(2, 2, 10, 10), -angle_ * 16, 270 * 16);
    }
private:
    QTimer timer_;
    int angle_ = 0;
};

namespace {
/// Drawn over the whole panel, letting the mouse through.
class DropOverlay : public QWidget {
public:
    using Paint = std::function<void(QPainter&, const QRectF&)>;
    DropOverlay(Paint paint, QWidget* parent) : QWidget(parent), paint_(std::move(paint)) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paint_(p, QRectF(rect()));
    }
private:
    Paint paint_;
};
} // namespace

LibraryPanel::LibraryPanel(Controller& c, QWidget* parent) : QWidget(parent), c_(c) {
    setAcceptDrops(true);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    header_ = new QWidget;
    header_->setFixedHeight(44);
    auto* h = new QHBoxLayout(header_);
    h->setContentsMargins(16, 0, 12, 0);
    h->setSpacing(6);
    auto* title = new MediaTitle(c_, header_);
    title->setObjectName(QStringLiteral("mediaTitle"));
    h->addWidget(title);
    auto* spinner = new Spinner(header_);
    spinner->setObjectName(QStringLiteral("importSpinner"));
    h->addWidget(spinner);
    h->addStretch(1);
    h->addWidget(new AddButton(c_, QStringLiteral("textframe"), DragPayload::text(), QStringLiteral("Text: click to add at the playhead, or drag to the timeline"), header_));
    h->addWidget(new AddButton(c_, QStringLiteral("solid"), DragPayload::solid(), QStringLiteral("Solid: click to add at the playhead, or drag to the timeline"), header_));
    auto* import = new Painted(QSize(26, 26), [](QPainter& p, const QRectF& r, bool hovering) {
        if (hovering) { p.setPen(Qt::NoPen); p.setBrush(Theme::hover()); p.drawRoundedRect(r, 7, 7); }
        Symbols::draw(p, QStringLiteral("square.and.arrow.down"), r.center(), 26 * 0.46, hovering ? Theme::text() : Theme::textDim(), 500);
    }, header_);
    import->setToolTip(QStringLiteral("Import Media (Ctrl+I)"));
    connect(import, &QAbstractButton::clicked, this, [this] { c_.showImportPanel(); });
    h->addWidget(import);
    v->addWidget(header_);

    scroll_ = new QScrollArea;
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    body_ = new QWidget;
    auto* bv = new QVBoxLayout(body_);
    bv->setContentsMargins(12, 0, 12, 12);
    bv->setSpacing(12);
    // Inside a linked folder that can't be found: where to find it.
    missing_ = new QWidget;
    missing_->setObjectName(QStringLiteral("missingFolder"));
    missing_->setAttribute(Qt::WA_StyledBackground);
    missing_->setStyleSheet(QStringLiteral("#missingFolder { background: %1; border-radius: 7px; }").arg(Theme::raised().name()));
    {
        auto* mh = new QHBoxLayout(missing_);
        mh->setContentsMargins(9, 7, 9, 7);
        mh->setSpacing(7);
        auto* warn = new Painted(QSize(12, 12), [](QPainter& p, const QRectF& r, bool) {
            Symbols::draw(p, QStringLiteral("exclamationmark.triangle.fill"), r.center(), 10.5, yellow(), 500);
        }, missing_);
        warn->setAttribute(Qt::WA_TransparentForMouseEvents);
        mh->addWidget(warn);
        auto* text = new QLabel;
        text->setObjectName(QStringLiteral("missingText"));
        text->setFont(Theme::font(11));
        text->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Theme::text().name()));
        mh->addWidget(text, 1);
        auto* locate = new Painted(QSize(60, 20), [](QPainter& p, const QRectF& r, bool) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::text());
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
            p.setFont(Theme::font(11, 500));
            p.setPen(Theme::onText());
            p.drawText(r, Qt::AlignCenter, QStringLiteral("Locate…"));
        }, missing_);
        connect(locate, &QAbstractButton::clicked, this, [this] {
            const auto& f = c_.model().mediaFolder;
            if (!f) return;
            if (const MediaFolder* root = MediaTree(c_.model().project()).root(*f)) c_.locateFolder(root->id);
        });
        mh->addWidget(locate);
    }
    bv->addWidget(missing_);
    search_ = new SearchField(body_);
    search_->setObjectName(QStringLiteral("mediaSearch"));
    bv->addWidget(search_);
    empty_ = new QLabel(QStringLiteral("Imported media\nappears here"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setFont(Theme::font(11.5));
    empty_->setStyleSheet(QStringLiteral("color: %1; background: transparent; padding-top: 16px;").arg(Theme::textFaint().name()));
    bv->addWidget(empty_);
    grid_ = new MediaGrid(c_, body_);
    bv->addWidget(grid_);
    noMatch_ = new QLabel;
    noMatch_->setAlignment(Qt::AlignCenter);
    noMatch_->setFont(Theme::font(11.5));
    noMatch_->setStyleSheet(QStringLiteral("color: %1; background: transparent; padding-top: 8px;").arg(Theme::textFaint().name()));
    bv->addWidget(noMatch_);
    bv->addStretch(1);
    scroll_->setWidget(body_);
    v->addWidget(scroll_, 1);

    // Files dragged over the panel, or anywhere else in the window that imports: they'll land here.
    dropOverlay_ = new DropOverlay([](QPainter& p, const QRectF& r) {
        // What happens, on a dark fade up from the bottom, so it reads over any pictures and the media stays in view.
        const double fadeH = std::min(150.0, r.height());
        QLinearGradient fade(QPointF(0, r.bottom()), QPointF(0, r.bottom() - fadeH));
        fade.setColorAt(0, QColor(0, 0, 0, int(0.92 * 255)));
        fade.setColorAt(0.45, QColor(0, 0, 0, int(0.7 * 255)));
        fade.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(r.left(), r.bottom() - fadeH, r.width(), fadeH), fade);
        const QColor ink(0xF2, 0xF2, 0xF0);
        p.setFont(Theme::font(12, 600));
        const QString label = QStringLiteral("Add to Media");
        const double tw = QFontMetricsF(p.font()).horizontalAdvance(label);
        const double total = 11 + 6 + tw, x = r.center().x() - total / 2, y = r.bottom() - 22 - 8;
        Symbols::draw(p, QStringLiteral("arrow.down.to.line"), QPointF(x + 5.5, y), 11, ink, 600);
        p.setPen(ink);
        p.drawText(QRectF(x + 17, y - 9, tw + 2, 18), Qt::AlignVCenter | Qt::AlignLeft, label);
        p.setPen(QPen(alpha(Theme::accent(), 0.9), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -1, -1));
    }, this);
    dropOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    dropOverlay_->hide();
    connect(&c_, &Controller::importDropHoverChanged, this, [this] { showDropped(); });
    connect(&c_.model(), &EditorModel::mediaRevealRequested, this, &LibraryPanel::reveal);

    connect(search_, &QLineEdit::textChanged, grid_, &MediaGrid::setSearch);
    connect(grid_, &MediaGrid::contentChanged, this, &LibraryPanel::refresh);
    connect(grid_, &MediaGrid::folderOpened, search_, &QLineEdit::clear);
    connect(&c_, &Controller::importChanged, this, [this, spinner] { spinner->setRunning(c_.importCount() > 0); refresh(); });
    connect(&c_.model(), &EditorModel::mediaSelectionChanged, this, [this] { refresh(); });
    spinner->setRunning(false);
    refresh();
}

void LibraryPanel::refresh() {
    const Project& p = c_.model().project();
    const MediaTree tree(p);
    const auto& folder = c_.model().mediaFolder;
    const bool inFolder = folder && tree.exists(*folder);
    if (auto* title = findChild<QWidget*>(QStringLiteral("mediaTitle"))) static_cast<MediaTitle*>(title)->refresh();
    // A missing linked folder, when one is open.
    const MediaFolder* root = inFolder ? tree.root(*folder) : nullptr;
    const bool missing = root && !MediaFolders::isFolder(root->path);
    missing_->setVisible(missing);
    if (missing) findChild<QLabel*>(QStringLiteral("missingText"))->setText(QStringLiteral("“%1” can't be found").arg(root->name()));
    const bool nothing = p.assets.empty() && tree.folders(std::nullopt).isEmpty();
    empty_->setVisible(nothing);
    grid_->setVisible(!nothing);
    search_->setPlaceholderText(inFolder ? QStringLiteral("Search folder") : QStringLiteral("Search"));
    const QString s = search_->text();
    noMatch_->setVisible(!nothing && grid_->empty());
    noMatch_->setText(s.contains(u'"') || s.contains(QChar(0x201C)) ? QStringLiteral("Nothing says %1").arg(s) : QStringLiteral("No media matches “%1”").arg(s));
}

void LibraryPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::panel());
}

void LibraryPanel::showDropped() {
    const bool on = dropTargeted_ || c_.importDropHover();
    if (on) {
        dropOverlay_->setGeometry(rect());
        dropOverlay_->raise();
    }
    dropOverlay_->setVisible(on);
}

void LibraryPanel::reveal(const MediaPick& target) {
    // Just imported: scrolled into view (after the folder it's in has opened and drawn).
    search_->clear();
    QTimer::singleShot(0, this, [this, target] {
        grid_->relayout();
        for (const auto& it : grid_->items()) {
            if (!(it.pick() == target)) continue;
            const QPoint centre = grid_->mapTo(body_, it.rect.center().toPoint());
            QScrollBar* bar = scroll_->verticalScrollBar();
            const int to = std::clamp(centre.y() - scroll_->viewport()->height() / 2, bar->minimum(), bar->maximum());
            auto* anim = new QPropertyAnimation(bar, "value", bar);
            anim->setDuration(250);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            anim->setStartValue(bar->value());
            anim->setEndValue(to);
            anim->start(QAbstractAnimation::DeleteWhenStopped);
            return;
        }
    });
}

void LibraryPanel::panelMenu(Controller& c, QWidget* parent, QPoint at) {
    EditorModel& m = c.model();
    QMenu menu(parent);
    menu.addAction(QStringLiteral("Add Text"), parent, [&m] { m.addAtPlayhead(DragPayload::text()); });
    menu.addAction(QStringLiteral("Add Solid"), parent, [&m] { m.addAtPlayhead(DragPayload::solid()); });
    menu.addAction(QStringLiteral("Import Media…"), parent, [&c] { c.showImportPanel(); });
    menu.addSeparator();
    menu.addSection(QStringLiteral("View as"));
    auto* group = new QActionGroup(&menu);
    for (auto [l, name] : {std::make_pair(MediaLayout::list, QStringLiteral("List")), std::make_pair(MediaLayout::thumbnails, QStringLiteral("Thumbnails"))}) {
        QAction* a = menu.addAction(name);
        a->setCheckable(true);
        a->setChecked(mediaLayout() == l);
        group->addAction(a);
        const MediaLayout layout = l;
        QObject::connect(a, &QAction::triggered, parent, [&m, layout] { setMediaLayout(layout); emit m.mediaSelectionChanged(); });
    }
    menu.exec(at);
}

void LibraryPanel::contextMenuEvent(QContextMenuEvent* e) {
    if (scroll_->geometry().contains(e->pos())) clearPicks(c_.model());
    panelMenu(c_, this, e->globalPos());
}

void LibraryPanel::mousePressEvent(QMouseEvent* e) {
    // Below the items: lets go of what's picked, as between them.
    if (e->button() == Qt::LeftButton && scroll_->geometry().contains(e->position().toPoint())) clearPicks(c_.model());
}

void LibraryPanel::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasUrls()) return;
    dropTargeted_ = true;
    showDropped();
    e->acceptProposedAction();
}

void LibraryPanel::dragLeaveEvent(QDragLeaveEvent*) {
    dropTargeted_ = false;
    showDropped();
}

void LibraryPanel::dropEvent(QDropEvent* e) {
    dropTargeted_ = false;
    c_.setImportDropHover(false);
    showDropped();
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
    c_.importFiles(paths);
    e->acceptProposedAction();
}

} // namespace er
