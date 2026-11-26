#include "Waveform.h"
#include "media/Decoder.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <cstring>

namespace er {

QString WaveformReader::folder() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/Editroom/Waveforms");
    QDir().mkpath(dir);
    return dir;
}

QString WaveformReader::cacheName(const QString& source) {
    QFileInfo fi(source);
    // Swift interpolates the Date as "2026-11-25 19:33:00 +0000".
    const QString date = fi.exists() ? fi.lastModified().toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QStringLiteral(" +0000")
                                     : QStringLiteral("0001-01-01 00:00:00 +0000");
    const QByteArray stamp = (source + u'|' + QString::number(fi.exists() ? fi.size() : 0) + u'|' + date).toUtf8();
    uint64_t hash = 0xcbf29ce484222325ull;
    for (char b : stamp) hash = (hash ^ uint8_t(b)) * 0x100000001b3ull;
    return fi.completeBaseName() + u'-' + QString::number(hash, 36);
}

QString WaveformReader::cachePath(const QString& source) { return folder() + u'/' + cacheName(source) + QStringLiteral(".wave"); }

static const uint32_t kMagic = 0x46575245; // "ERWF"
static const uint32_t kVersion = 1;

bool WaveformReader::save(const Waveform& wf, const QString& path) {
    QByteArray d;
    auto put = [&](const void* p, size_t n) { d.append(static_cast<const char*>(p), qsizetype(n)); };
    const uint32_t n = uint32_t(wf.peaks.size());
    put(&kMagic, 4); put(&kVersion, 4); put(&wf.peaksPerSecond, 8); put(&n, 4);
    for (const auto* arr : {&wf.peaks, &wf.left, &wf.right, &wf.rms}) {
        if (arr->size() != wf.peaks.size()) return false;
        put(arr->data(), arr->size() * 4);
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(d);
    return f.commit();
}

std::optional<Waveform> WaveformReader::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray d = f.readAll();
    if (d.size() < 20) return std::nullopt;
    uint32_t magic, version, n;
    double pps;
    std::memcpy(&magic, d.constData(), 4);
    std::memcpy(&version, d.constData() + 4, 4);
    std::memcpy(&pps, d.constData() + 8, 8);
    std::memcpy(&n, d.constData() + 16, 4);
    if (magic != kMagic || version != kVersion || n == 0 || d.size() != qsizetype(20 + size_t(n) * 16)) return std::nullopt;
    Waveform wf;
    wf.peaksPerSecond = pps;
    std::vector<float>* arrays[4] = {&wf.peaks, &wf.left, &wf.right, &wf.rms};
    for (int k = 0; k < 4; ++k) {
        arrays[k]->resize(n);
        std::memcpy(arrays[k]->data(), d.constData() + 20 + size_t(k) * n * 4, size_t(n) * 4);
    }
    return wf;
}

std::optional<Waveform> WaveformReader::read(const QString& source, double pps) {
    auto probe = Probe::file(source);
    if (!probe || !probe->hasAudio) return std::nullopt;
    const int rate = probe->audioRate > 0 ? probe->audioRate : 48000;
    auto dec = AudioDecoder::open(source, rate);
    if (!dec) return std::nullopt;
    Waveform wf;
    wf.peaksPerSecond = pps;
    const double framesPerBucket = rate / pps;
    float peak = 0, l = 0, r = 0, squares = 0;
    double fill = 0;
    std::vector<float> buf;
    while (true) {
        buf.clear();
        const int n = dec->read(buf, 65536);
        if (n == 0) break;
        for (int i = 0; i < n; ++i) {
            const float a = std::abs(buf[size_t(i) * 2]), b = std::abs(buf[size_t(i) * 2 + 1]);
            const float m = std::max(a, b);
            peak = std::max(peak, m);
            squares += m * m;
            l = std::max(l, a);
            r = std::max(r, b);
            fill += 1;
            if (fill >= framesPerBucket - 1e-6) {
                wf.peaks.push_back(peak);
                wf.rms.push_back(std::sqrt(squares / float(std::max(1.0, fill))));
                wf.left.push_back(l);
                wf.right.push_back(r);
                peak = l = r = squares = 0;
                fill -= framesPerBucket;
            }
        }
    }
    if (wf.peaks.empty()) return std::nullopt;
    return wf;
}

std::optional<Waveform> WaveformReader::forFile(const QString& source) {
    const QString cache = cachePath(source);
    if (auto wf = load(cache)) return wf;
    auto wf = read(source);
    if (wf) save(*wf, cache);
    return wf;
}

} // namespace er
