// TimelineCanvas.swift: the timeline drawn with QPainter, with the Mac's geometry and every interaction.
//   Ruler: click or drag to scrub (with its time beside the playhead), drag a marker's flag (a click opens its
//   panel), drag either end of the in/out range.
//   Caption track: click to pick captions (Shift adds), drag to box more in, double-click to retype one.
//   Clips: click to select (Shift adds or takes away), drag to move (Alt drops copies; across tracks, with
//   linked sound following into free space), edges to trim (every selected clip's same edge, sticky or free),
//   fade dots to set fades, the level line of a sound clip to set its gain (double-click resets), the cut
//   tool to split. Empty space: drag a box to pick clips.
//   Snapping to clip edges, markers and the playhead (Ctrl turns it the other way while held), autoscroll at
//   the edges while dragging, the wheel to scroll (Alt or Ctrl to zoom), hover to skim, right-click menus,
//   and media dropped from the Media panel or the file manager.
// What's being dragged is drawn from overrides, not live edits: the project changes once, when the drag ends.
#pragma once
#include "Controller.h"
#include <QTimer>
#include <QWidget>
#include <map>

namespace er {

class MarkerPanel;
}
class QMimeData;
namespace er {

class TimelineCanvas : public QWidget {
    Q_OBJECT
public:
    TimelineCanvas(Controller& c, QWidget* parent = nullptr);
    ~TimelineCanvas() override;
    double x(double time) const { return time * view().pps - view().scrollX; }
    double time(double x) const { return (x + view().scrollX) / view().pps; }
    /// Where a clip is drawn (none for a track not shown).
    std::optional<QRectF> rect(const Clip& c) const;
    /// The timeline on screen, for Magic's cursors (none when there isn't one).
    static TimelineCanvas* current();
    /// The wheel and pinch from the track headers beside it scroll and zoom it too.
    void scrollBy(QWheelEvent* e) { wheelEvent(e); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    enum class Zone { body, left, right, fadeIn, fadeOut, gain };
    struct Hit { Clip clip; Zone zone; QRectF rect; };
    struct Lane {
        enum class Kind { track, newVideo, newAudio, none } kind = Kind::none;
        Track track;
        int index = 0;
    };
    struct Drag {
        enum class Kind { none, scrub, pendingMove, move, trim, fade, gain, marquee, captionMarquee, marker, rangeEdge } kind = Kind::none;
        Clip grabbed;
        QPointF point;
        double time = 0;
        std::vector<Clip> originals;
        bool left = false;          // trim
        Uuid id;                    // fade, gain, marker
        bool fadeIn = false;
        double startY = 0, startNorm = 0, height = 0; // gain
        QPointF origin;             // marquee (content coordinates)
        IdSet base;
        std::set<QString> captionBase;
        double startX = 0;          // marker
        bool moved = false;
        bool isIn = false;          // range edge
    };

    const TimelineViewState& view() const { return c_.model().view(); }
    double pps() const { return view().pps; }
    std::optional<QRectF> captionRect(const CaptionBlock& b) const;
    bool inCaptionRow(QPointF p) const;
    std::optional<CaptionBlock> captionHit(QPointF p) const;
    Lane lane(double y) const;
    bool snappingActive() const;
    std::optional<std::pair<double, double>> snap(const std::vector<double>& candidates, const std::vector<double>& targets, double pixels = 8) const;
    double fadeHandleX(const Clip& c, const QRectF& r, bool fadeIn) const;
    static double gainNorm(double db);
    static double gainDB(double norm);
    std::optional<Hit> hit(QPointF p) const;
    void updateHover(QPointF p);
    void clearHover();
    void handleDrag(QPointF p);
    void press(QMouseEvent* e, int clickCount);
    void scrub(QPointF p);
    void updateMove(const Clip& grabbed, double startTime, const std::vector<Clip>& originals, QPointF p);
    void updateTrim(const Clip& grabbed, bool left, double startTime, const std::vector<Clip>& originals, QPointF p);
    void updateStickyTrim(const Clip& grabbed, bool left, double startTime, const std::vector<Clip>& originals, QPointF p);
    void showTrimFrame(const Clip& grabbed, bool left);
    static bool attachedOnLeft(const std::vector<Clip>& clips, const Project& project);
    void updateAutoscroll(QPointF p);
    void stopAutoscroll();
    std::vector<DragPayload> payloads(const QMimeData* mime) const;
    bool updateDrop(const QMimeData* mime, QPointF p);
    void clearDrop();
    void showMarkerPanel();
    void placePlayhead();

    void drawClip(QPainter& ctx, const Clip& c, double alpha);
    void drawCaptions(QPainter& ctx, const QRectF& lane, bool dim);
    std::vector<CaptionBlock> shownCaptions() const;
    void drawRuler(QPainter& ctx, const Project& project);
    void drawFilmstrip(QPainter& ctx, const Clip& c, const MediaAsset& asset, const QRectF& r, const QRectF& visible);
    void drawWaveform(QPainter& ctx, const Clip& c, const QRectF& r, const QRectF& visible);
    static void drawImage(QPainter& ctx, const QImage& img, const QRectF& fill);
    static void drawGradeWheel(QPainter& ctx, QPointF centre, double radius);
    QImage graded(const QImage& img, const std::optional<ColorGrade>& grade);
    std::optional<Uuid> markerHit(QPointF p) const;
    std::optional<bool> rangeEdgeHit(QPointF p) const;
    double markerLabelWidth(const Marker& m) const;

    Controller& c_;
    QWidget* playhead_;
    TimelineLayout layout_;
    Drag drag_;
    std::map<Uuid, Clip> overrides_;
    std::vector<Clip> ghosts_;
    std::optional<std::pair<Uuid, Zone>> hover_;
    std::optional<double> snapLine_;
    std::optional<double> scrubReadout_;
    /// Alt held during a move: drop copies instead of moving the clips.
    bool duplicating_ = false;
    std::optional<double> lastScrubSnap_;
    std::optional<std::pair<Uuid, double>> cutHover_;
    std::optional<QRectF> marquee_;
    std::optional<std::pair<QString, QPointF>> bubble_;
    int moveTrackDelta_ = 0;
    struct DropInfo { Uuid video, audio; double start; };
    std::optional<DropInfo> dropInfo_;
    QPointF lastDragPoint_;
    QTimer autoscroll_;
    double autoscrollVelocity_ = 0;
    Qt::KeyboardModifiers modifiers_;
    QHash<QString, QImage> gradedThumbs_;
    MarkerPanel* markerPanel_ = nullptr;
};

} // namespace er
