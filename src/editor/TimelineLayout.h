// TimelineLayout.swift: the timeline's vertical arrangement, shared by the canvas and the track headers, and
// TimelineViewState (EditorModel.swift): zoom, scroll and row height, with every limit the canvas, the scroll
// bars and the wheel agree on.
#pragma once
#include "model/Project.h"
#include <optional>

namespace er {

struct TimelineLayout {
    static constexpr double ruler = 26, zone = 26, gap = 1, divider = 9, headerWidth = 176;
    /// Height everything except the track rows takes up.
    static constexpr double fixedHeight = zone * 2 + divider;
    static constexpr double captionRow = 30;
    /// Height the caption track takes above the video tracks (none without one).
    static double captionSpace(const Project& p) { return p.captions ? captionRow + gap : 0; }

    struct Row {
        Track track;
        int index;
        double y, height;
    };
    std::vector<Row> rows;
    /// The caption track's row, at the top, when the sequence has one.
    std::optional<double> captionY;
    double videoZoneY = 0, dividerY = 0, audioZoneY = 0, contentHeight = 0;

    TimelineLayout(const Project& project, double rowHeight) {
        double y = 0;
        if (project.captions) {
            captionY = y;
            y += captionRow + gap;
        }
        videoZoneY = y;
        y += zone;
        for (int i = int(project.videoTracks.size()) - 1; i >= 0; --i) {
            rows.push_back({project.videoTracks[size_t(i)], i, y, rowHeight});
            y += rowHeight + gap;
        }
        dividerY = y;
        y += divider;
        for (size_t i = 0; i < project.audioTracks.size(); ++i) {
            rows.push_back({project.audioTracks[i], int(i), y, rowHeight});
            y += rowHeight + gap;
        }
        audioZoneY = y;
        y += zone;
        contentHeight = y;
    }

    /// Vertical band (content coordinates) for a track id, including the "new track" placeholders.
    std::optional<std::pair<double, double>> band(const Uuid& trackID) const {
        if (trackID == TrackToken::newVideo()) return std::make_pair(videoZoneY + 2, zone - 4);
        if (trackID == TrackToken::newAudio()) return std::make_pair(audioZoneY + 2, zone - 4);
        for (const auto& r : rows) if (r.track.id == trackID) return std::make_pair(r.y, r.height);
        return std::nullopt;
    }
};

struct TimelineViewState {
    static constexpr double minPPS = 0.05, maxPPS = 2400;
    static constexpr double rowMin = 30, rowMax = 110;
    double pps = 80;
    double scrollX = 0, scrollY = 0;
    double rowHeight = 62;
    double viewWidth = 900, viewHeight = 300;
    /// Set until the timeline has reported its real size and the tracks have been centred in it.
    bool needsInitialCentering = true;
};

/// RangeBarStyle: the scroll bars' thickness and their grips.
struct RangeBarStyle {
    static constexpr double thickness = 15, handle = 11;
};

} // namespace er
