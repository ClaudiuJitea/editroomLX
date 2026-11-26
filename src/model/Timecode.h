// Timecode and TimeStyle (Project.swift).
#pragma once
#include <QString>
#include <optional>

namespace er {

/// How times are shown (Settings > General): full timecode with frames, or plain minutes and seconds.
enum class TimeStyle { timecode, simple };

namespace Timecode {
/// The settings key, the same as the Mac's.
inline constexpr const char* styleKey = "timeStyle";
/// The style used by string()/parse(). The app sets it from settings; tests set it directly.
TimeStyle currentStyle();
void setCurrentStyle(TimeStyle);

/// A time as the person chose to see times: "00:00:05:15", or "0:05.50".
QString string(double t, double fps);
/// Minutes, seconds and hundredths ("0:05.50"), with hours when there are any ("1:02:05.50").
QString simple(double t);
QString full(double t, double fps);
/// Reads a typed timecode. "1:02:03" counts from the right (frames, seconds, minutes, hours);
/// plain digits are read in pairs from the right, so "1000" is 10 seconds.
std::optional<double> parse(const QString& s, double fps);
QString shortString(double t);
} // namespace Timecode

} // namespace er
