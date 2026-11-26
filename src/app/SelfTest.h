// SelfTest.swift and FormatCheck.swift, the headless engine checks:
//   editroom-lx --selftest <mediaDir> <outDir>   imports every file, builds a small multi-track edit (speed, a dissolve,
//                                                a graded section, a solid, a title, a picture on V2, a tone), saves it
//                                                and exports it as MP4 (H.264, HEVC web 4K, no audio) and WAV.
//   editroom-lx --format-check <outDir> <file>…   imports each file and reports what was made of it, then checks a
//                                                frame from playback, a reversed clip, the preview and an export;
//                                                patch clips (sdr_, hlg_, pq_, applelog_) are compared colour by colour.
// The Mac's SelfTest goes on through many more engine checks; LX runs those as its unit tests (ctest).
#pragma once
#include <QStringList>

namespace er {
int runSelfTest(const QString& mediaDir, const QString& outDir);
int runFormatCheck(const QString& outDir, const QStringList& files);
} // namespace er
