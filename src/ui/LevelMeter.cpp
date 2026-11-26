#include "LevelMeter.h"
#include "Theme.h"
#include <QPainter>

namespace er {

LevelMeter::LevelMeter(Controller& c, std::optional<Uuid> track, Side side, QWidget* parent)
    : QWidget(parent), c_(c), track_(track), side_(side) {
    setFixedWidth(track ? 6 : 4);
    connect(&c_, &Controller::metersChanged, this, qOverload<>(&QWidget::update));
}

void LevelMeter::paintEvent(QPaintEvent*) {
    constexpr double segment = 2, gap = 1, floorDB = MeterReading::floorDB;
    MeterReading r;
    if (track_) {
        auto it = c_.meters().tracks().find(*track_);
        if (it != c_.meters().tracks().end()) r = it->second;
    } else {
        r = side_ == Side::left ? c_.meters().left() : side_ == Side::right ? c_.meters().right() : c_.meters().master();
    }
    setToolTip(track_ ? (r.clipped ? QStringLiteral("Clipping") : QStringLiteral("Level")) : QString());
    QPainter p(this);
    const int count = std::max(4, int((height() + gap) / (segment + gap)));
    // The top segment is the clip light; the rest span the floor to 0 dB.
    const int span = count - 1;
    const int lit = int(std::clamp(span * ((r.level - floorDB) / -floorDB), 0.0, double(span)));
    const int peak = int(std::clamp(span * ((r.peak - floorDB) / -floorDB), 0.0, double(span))) - 1;
    auto color = [](double db) { return db > -3 ? Theme::alert() : db > -12 ? Theme::snap() : Theme::hex(Theme::p().meterLow); };
    // Bottom aligned: the segments stack up from the bottom edge.
    double y = height() - segment;
    for (int i = 0; i < span; ++i) {
        const bool on = i < lit || (i == peak && r.peak > floorDB + 0.5);
        p.fillRect(QRectF(0, y, width(), segment), on ? color(floorDB * (1 - double(i + 1) / span)) : Theme::well());
        y -= segment + gap;
    }
    p.fillRect(QRectF(0, y, width(), segment), r.clipped ? Theme::alert() : Theme::well());
}

} // namespace er
