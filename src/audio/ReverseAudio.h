// ReverseAudio (Reverser.swift): a file's whole sound written backwards as 16-bit stereo WAV at the file's
// own rate, made once per file (named from its path, size and date) and reused.
#pragma once
#include <QString>
#include <optional>

namespace er::ReverseAudio {
QString folder();
QString cachePath(const QString& source);
/// The reversed copy and its length, if it's been made.
std::optional<std::pair<QString, double>> existing(const QString& source);
/// Makes it (blocking); the error message on failure ("Couldn't read the sound.").
std::optional<std::pair<QString, double>> render(const QString& source, QString* error = nullptr);
} // namespace er::ReverseAudio
