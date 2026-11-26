// LevelMeter (TimelinePanel.swift): a vertical segmented level meter. Lit segments are light grey, yellow from
// -12 dB and red from -3 dB; one segment briefly holds the recent peak; the top segment is red while the signal
// is at full scale. Everything empties the moment playback stops. A track's, or the output's (both sides, or one).
#pragma once
#include "Controller.h"
#include <QWidget>

namespace er {

class LevelMeter : public QWidget {
public:
    enum class Side { both, left, right };
    LevelMeter(Controller& c, std::optional<Uuid> track, Side side = Side::both, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Controller& c_;
    std::optional<Uuid> track_;
    Side side_;
};

} // namespace er
