#include "ReverseAudio.h"
#include "Waveform.h"
#include "media/Decoder.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <cmath>
#include <cstring>

namespace er::ReverseAudio {

QString folder() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/Editroom/Reversed");
    QDir().mkpath(dir);
    return dir;
}

QString cachePath(const QString& source) { return folder() + u'/' + WaveformReader::cacheName(source) + QStringLiteral(".wav"); }

static double wavLength(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 44) return 0;
    const QByteArray h = f.read(44);
    uint32_t rate, bytes;
    std::memcpy(&rate, h.constData() + 24, 4);
    std::memcpy(&bytes, h.constData() + 40, 4);
    return rate ? double(bytes / 4) / rate : 0;
}

std::optional<std::pair<QString, double>> existing(const QString& source) {
    const QString p = cachePath(source);
    if (!QFileInfo::exists(p)) return std::nullopt;
    return std::make_pair(p, wavLength(p));
}

static QByteArray wavHeader(int64_t frames, int rate) {
    QByteArray d;
    auto u32 = [&](uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { d.append(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(frames * 4);
    d.append("RIFF"); u32(36 + bytes);
    d.append("WAVE");
    d.append("fmt "); u32(16); u16(1); u16(2); u32(uint32_t(rate)); u32(uint32_t(rate * 4)); u16(4); u16(16);
    d.append("data"); u32(bytes);
    return d;
}

std::optional<std::pair<QString, double>> render(const QString& source, QString* error) {
    auto fail = [&] { if (error) *error = QStringLiteral("Couldn't read the sound."); return std::nullopt; };
    auto probe = Probe::file(source);
    if (!probe || !probe->hasAudio) return fail();
    // Kept at the file's own rate, so nothing is resampled.
    const int rate = probe->audioRate >= 8000 ? probe->audioRate : 48000;
    auto dec = AudioDecoder::open(source, rate);
    if (!dec) return fail();
    // The decoded sound goes to a scratch file first and is written out backwards from there, so memory
    // stays small even for long recordings.
    QTemporaryFile scratch;
    if (!scratch.open()) return fail();
    std::vector<float> buf;
    std::vector<int16_t> pcm;
    int64_t frames = 0;
    while (true) {
        buf.clear();
        const int n = dec->read(buf, 1 << 16);
        if (n == 0) break;
        pcm.resize(size_t(n) * 2);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(std::lround(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f));
        scratch.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * 2));
        frames += n;
    }
    scratch.flush();
    const QString out = cachePath(source);
    QFile file(out + QStringLiteral(".part"));
    if (!file.open(QIODevice::WriteOnly)) return fail();
    file.write(wavHeader(frames, rate));
    uchar* src = scratch.map(0, frames * 4);
    if (!src && frames > 0) return fail();
    // Backwards, a block at a time: each frame is 4 bytes (left and right).
    const int64_t block = 1 << 18;
    std::vector<uint32_t> tmp(static_cast<size_t>(block));
    const uint32_t* frame = reinterpret_cast<const uint32_t*>(src);
    for (int64_t end = frames; end > 0;) {
        const int64_t start = std::max<int64_t>(0, end - block);
        const int64_t n = end - start;
        for (int64_t i = 0; i < n; ++i) tmp[size_t(i)] = frame[end - 1 - i];
        file.write(reinterpret_cast<const char*>(tmp.data()), n * 4);
        end = start;
    }
    file.close();
    QFile::remove(out);
    QFile::rename(out + QStringLiteral(".part"), out);
    return std::make_pair(out, double(frames) / rate);
}

} // namespace er::ReverseAudio
