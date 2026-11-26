// `editroom-lx --bench <dir>`: the standard measurements (IMPLEMENTATION.md §12), each the median of 5
// runs with the spread, as `BENCH` lines. The media is generated into <dir> with the system ffmpeg.
#pragma once
#include <QString>
namespace er { int runBench(const QString& dir); }
