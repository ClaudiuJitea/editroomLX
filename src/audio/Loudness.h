// Loudness.swift: integrated loudness (ITU-R BS.1770, LUFS) and sample peak of a mix, ported verbatim:
// K-weighting at 48 kHz, 100 ms steps, 400 ms blocks with 75% overlap, −70 LUFS absolute gate and a
// relative gate 10 LU below.
#pragma once
#include <optional>
#include <vector>

namespace er::LoudnessMeter {
struct Result { double lufs = 0; double peakDB = -120; };
/// `stereo` is interleaved L/R at 48 kHz.
std::optional<Result> measure(const std::vector<float>& stereo);
/// The export loudness target's gain (Exporter): min(target − lufs, −1 − peak).
double targetGain(const Result& r, double targetLUFS);
} // namespace er::LoudnessMeter
