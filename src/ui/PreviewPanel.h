// PreviewView.swift: the viewer and the transport under it.
//   The viewer: the composed frame at the sequence's shape (rounded 3, a faint rim), fitted, or at 25–200% with
//   scrolling when it's bigger than the space. Dragging in it moves the one picked picture (a title, or a clip's
//   framing, snapping back to centre). On an empty timeline it says how to start, and a click imports. A library
//   item being hover-scrubbed, or a trimmed edge's frame, shows over it, labelled.
//   The transport: the time (click to type one), the length; previous edit, back a frame, play, forward a frame,
//   next edit; loop, the output meters, the viewer zoom, the full-screen viewer.
#pragma once
#include <QVulkanInstance>
#include <QTimer>
#include <QWindow>
#include <memory>
#include "Controller.h"
#include <QWidget>

class QScrollArea;

namespace er {

class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(Controller& c, QWidget* parent = nullptr);
    void setFrame(const QImage& img) { frame_ = img; update(); }
    /// The picture's place in the widget.
    QRectF picture() const;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override { promptHover_ = false; update(); }
    void resizeEvent(QResizeEvent*) override { updatePresenting(); }
    void showEvent(QShowEvent*) override { updatePresenting(); }
    void hideEvent(QHideEvent*) override { updatePresenting(); }
public:
    /// The picture straight from the GPU (a Vulkan window over it) when nothing has to be painted on top: else the
    /// read-back frame is painted. EDITROOM_PRESENT=0 always paints.
    void updatePresenting();
    bool presenting() const;
private:
    std::unique_ptr<QVulkanInstance> vulkan_;
    QWindow* present_ = nullptr;
    QWidget* presentBox_ = nullptr;
    /// The one visual clip in the selection, which dragging in the viewer moves.
    std::optional<Clip> target() const;
    void drawEmptyPrompt(QPainter& p, const QRectF& r);
    QRectF promptRect() const;
    Controller& c_;
    QImage frame_;
    std::optional<std::pair<double, double>> moveStart_;
    QPointF press_;
    bool dragging_ = false;
    bool promptHover_ = false;
};

class TransportBar;

class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(Controller& c, QWidget* parent = nullptr);
    Viewer* viewer() const { return viewer_; }
    /// The viewer's zoom: none fits the frame to the space.
    void setZoom(std::optional<double> z);
    std::optional<double> zoom() const { return zoom_; }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void place();
    Controller& c_;
    QScrollArea* area_;
    Viewer* viewer_;
    TransportBar* bar_;
    std::optional<double> zoom_;
};

} // namespace er
