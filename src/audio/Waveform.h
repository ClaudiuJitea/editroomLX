// Waveforms (MediaStore.swift WaveformReader): per 1/200 s of a file's sound, the loudest sample of the
// loudest channel, of each side, and the RMS. Kept on disk in the Mac's "ERWF" layout (version 1), in
// $XDG_CACHE_HOME/Editroom/Waveforms, named from the file's path, size and date.
#pragma once
#include <QString>
#include <optional>
#include <vector>

namespace er {

struct Waveform {
    double peaksPerSecond = 200;
    std::vector<float> peaks, left, right, rms;
};

namespace WaveformReader {
QString folder();
/// ReverseAudio.cacheName: the file's name plus an FNV-1a hash (base 36) of its path, size and date.
QString cacheName(const QString& sourcePath);
QString cachePath(const QString& sourcePath);
bool save(const Waveform& wf, const QString& path);
std::optional<Waveform> load(const QString& path);
/// Decodes the sound once, at the file's own rate, and sums it into short stretches.
std::optional<Waveform> read(const QString& sourcePath, double peaksPerSecond = 200);
/// From the cache, or read and cached.
std::optional<Waveform> forFile(const QString& sourcePath);
} // namespace WaveformReader

} // namespace er
