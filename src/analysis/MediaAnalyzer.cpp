#include "MediaAnalyzer.h"
#include "audio/Waveform.h"
#include "engine/Faces.h"
#include "engine/SceneTagStore.h"
#include "media/Decoder.h"
#include "media/Stills.h"
#include "model/Json.h"
#include "ui/Thumbnails.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <complex>

namespace er {

namespace {

QString folder() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/Editroom/Analysis");
    QDir().mkpath(d);
    return d;
}
/// A cache file named for the source file's path, size and date (LX's own: "-lx", never mixed with a Mac's). A
/// clip's names how it was looked at too ("lx2": with faces; "a": with aesthetics), so a new model looks again.
QString cachePath(const QString& source, const char* kind) {
    const bool clip = qstrcmp(kind, "clip") == 0;
    const QString how = !clip ? QStringLiteral("lx") : SceneTagStore::available() ? QStringLiteral("lx2a") : QStringLiteral("lx2");
    return folder() + u'/' + WaveformReader::cacheName(source) + QStringLiteral("-%1-v%2-%3.json").arg(QLatin1String(kind)).arg(MediaAnalyzer::version).arg(how);
}
std::optional<QJsonObject> cached(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    if (!d.isObject()) return std::nullopt;
    return d.object();
}
void store(const QJsonObject& o, const QString& p) {
    QSaveFile f(p);
    if (f.open(QIODevice::WriteOnly)) { f.write(json::write(o, false)); f.commit(); }
}

QJsonObject toJson(const ClipAnalysis& a) {
    QJsonArray s;
    for (const auto& x : a.samples) {
        QJsonObject o{{"time", x.time}, {"luma", x.luma}, {"sharpness", x.sharpness}, {"motion", x.motion}, {"color", x.color}, {"faces", x.faces}};
        if (x.aesthetic) o.insert("aesthetic", *x.aesthetic);
        if (x.utility) o.insert("utility", *x.utility);
        s.append(o);
    }
    QJsonObject o{{"duration", a.duration}, {"samples", s}};
    if (a.created) o.insert("created", *a.created);
    return o;
}
ClipAnalysis clipFrom(const QJsonObject& o) {
    ClipAnalysis a;
    a.duration = o.value("duration").toDouble();
    if (o.contains("created")) a.created = o.value("created").toDouble();
    for (const auto& v : o.value("samples").toArray()) {
        const QJsonObject x = v.toObject();
        ClipAnalysis::Sample s{x.value("time").toDouble(), x.value("luma").toDouble(), x.value("sharpness").toDouble(),
                               x.value("motion").toDouble(), x.value("color").toDouble(), x.value("faces").toInt(), {}, {}};
        if (x.contains("aesthetic")) s.aesthetic = x.value("aesthetic").toDouble();
        if (x.contains("utility")) s.utility = x.value("utility").toBool();
        a.samples.push_back(s);
    }
    return a;
}

/// Brightness, detail, colour and change from one small frame to the next.
struct FrameMeasure {
    static constexpr int w = 160, h = 90;
    std::optional<std::vector<uint8_t>> previous;

    std::optional<ClipAnalysis::Sample> sample(const QImage& img, double time) {
        if (img.isNull()) return std::nullopt;
        const QImage g = img.convertToFormat(QImage::Format_RGB32).scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QImage c = img.convertToFormat(QImage::Format_RGB32).scaled(48, 27, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<uint8_t> gray(size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(g.constScanLine(y));
            for (int x = 0; x < w; ++x)
                gray[size_t(y) * w + x] = uint8_t(std::lround(0.299 * qRed(row[x]) + 0.587 * qGreen(row[x]) + 0.114 * qBlue(row[x])));
        }
        double sum = 0;
        for (uint8_t v : gray) sum += v;
        const double luma = sum / (w * h) / 255;
        // Laplacian variance: a standard measure of focus.
        std::vector<double> lap;
        lap.reserve(size_t(w - 2) * (h - 2));
        for (int y = 1; y < h - 1; ++y) {
            const int row = y * w;
            for (int x = 1; x < w - 1; ++x) {
                const double cc = double(gray[size_t(row + x)]) * 4;
                const double n = double(gray[size_t(row + x - 1)]) + gray[size_t(row + x + 1)] + gray[size_t(row - w + x)] + gray[size_t(row + w + x)];
                lap.push_back(n - cc);
            }
        }
        double mean = 0;
        for (double v : lap) mean += v;
        mean /= double(lap.size());
        double variance = 0;
        for (double v : lap) variance += (v - mean) * (v - mean);
        variance /= double(lap.size());
        double motion = 0;
        if (previous) {
            long long d = 0;
            for (size_t i = 0; i < gray.size(); ++i) d += std::abs(int(gray[i]) - int((*previous)[i]));
            motion = double(d) / (w * h) / 255;
        }
        previous = gray;
        // Colourfulness (Hasler and Süsstrunk).
        std::vector<double> rg, yb;
        for (int y = 0; y < 27; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(c.constScanLine(y));
            for (int x = 0; x < 48; ++x) {
                const double r = qRed(row[x]), gg = qGreen(row[x]), b = qBlue(row[x]);
                rg.push_back(r - gg);
                yb.push_back(0.5 * (r + gg) - b);
            }
        }
        auto stats = [](const std::vector<double>& a) {
            double m = 0;
            for (double v : a) m += v;
            m /= double(a.size());
            double s = 0;
            for (double v : a) s += (v - m) * (v - m);
            return std::make_pair(m, std::sqrt(s / double(a.size())));
        };
        const auto [mrg, srg] = stats(rg);
        const auto [myb, syb] = stats(yb);
        const double color = (std::sqrt(srg * srg + syb * syb) + 0.3 * std::sqrt(mrg * mrg + myb * myb)) / 110;
        return ClipAnalysis::Sample{time, luma, variance, motion, std::min(1.5, color), 0, std::nullopt, std::nullopt};
    }
};

/// vDSP_fft_zrip's packed real FFT of size n (a power of two), as magnitudes of bins 0…n/2−1, scaled as
/// vDSP scales it (twice the usual DFT).
void realFFTMagnitudes(const std::vector<float>& in, std::vector<float>& mag) {
    const size_t n = in.size();
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = in[i];
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    const size_t half = n / 2;
    mag.resize(half);
    // Bin 0 packs DC (real) and Nyquist (imaginary), as vDSP does.
    mag[0] = float(2 * std::abs(std::complex<double>(a[0].real(), a[half].real())));
    for (size_t k = 1; k < half; ++k) mag[k] = float(2 * std::abs(a[k]));
}

} // namespace

std::optional<ClipAnalysis> MediaAnalyzer::clip(const QString& path, bool isImage, int orientation) {
    const QString cache = cachePath(path, "clip");
    if (auto hit = cached(cache)) return clipFrom(*hit);
    ClipAnalysis result;
    const QFileInfo fi(path);
    if (fi.exists()) result.created = double(fi.birthTime().isValid() ? fi.birthTime().toSecsSinceEpoch() : fi.lastModified().toSecsSinceEpoch()) - 978307200.0;
    FrameMeasure measure;
    if (isImage) {
        auto img = Stills::load(path, 320);
        if (!img) return std::nullopt;
        auto s = measure.sample(*img, 0);
        if (!s) return std::nullopt;
        s->faces = Faces::count(*img);
        if (auto a = SceneTagStore::aesthetics(*img)) { s->aesthetic = a->score; s->utility = a->utility; }
        result.duration = 5;
        result.samples = {*s};
    } else {
        auto probe = Probe::file(path);
        if (!probe || !probe->hasVideo || !(probe->duration > 0.2)) return std::nullopt;
        const double duration = probe->duration;
        auto dec = VideoDecoder::open(path);
        if (!dec) return std::nullopt;
        const double step = std::max(0.25, duration / 60);
        for (double t = step / 2; t < duration - 0.04; t += step) {
            auto f = dec->frameAt(t);
            if (!f) continue;
            f->orientation = orientation;
            const QImage img = Thumbnails::fit(*f, probe->color, 320);
            auto s = measure.sample(img, t);
            if (!s) continue;
            // Faces on every other frame, the slower aesthetics check about once a second.
            const auto& done = result.samples;
            if (done.size() % 2 == 0) s->faces = Faces::count(img); else s->faces = done.empty() ? 0 : done.back().faces;
            if (done.size() % size_t(std::max(1, int(std::round(1 / step)))) == 0) {
                if (auto a = SceneTagStore::aesthetics(img)) { s->aesthetic = a->score; s->utility = a->utility; }
            } else if (!done.empty()) {
                s->aesthetic = done.back().aesthetic;
                s->utility = done.back().utility;
            }
            result.samples.push_back(*s);
        }
        if (result.samples.empty()) return std::nullopt;
        result.duration = duration;
    }
    store(toJson(result), cache);
    return result;
}

std::optional<MusicAnalysis> MediaAnalyzer::music(const QString& path) {
    const QString cache = cachePath(path, "beat");
    if (auto hit = cached(cache)) {
        MusicAnalysis m;
        m.duration = hit->value("duration").toDouble();
        m.bpm = hit->value("bpm").toDouble();
        m.barPhase = hit->value("barPhase").toInt();
        for (const auto& v : hit->value("beats").toArray()) m.beats.push_back(v.toDouble());
        for (const auto& v : hit->value("energy").toArray()) m.energy.push_back(float(v.toDouble()));
        return m;
    }
    auto dec = AudioDecoder::open(path, 22050);
    if (!dec) return std::nullopt;
    std::vector<float> stereo;
    while (dec->read(stereo, 1 << 16) > 0) {}
    std::vector<float> mono(stereo.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
    auto result = beats(mono, 22050);
    if (result) {
        QJsonArray b, e;
        for (double x : result->beats) b.append(x);
        for (float x : result->energy) e.append(double(x));
        store(QJsonObject{{"duration", result->duration}, {"bpm", result->bpm}, {"beats", b}, {"barPhase", result->barPhase}, {"energy", e}}, cache);
    }
    return result;
}

std::optional<MusicAnalysis> MediaAnalyzer::beats(const std::vector<float>& x, double rate) {
    const int n = 1024, hop = 512;
    // Finding a tempo needs a few seconds of sound.
    if (!(int(x.size()) > n * 8 && double(x.size()) > rate * 6)) return std::nullopt;
    // vDSP_hann_window with vDSP_HANN_NORM: 0.8165 × (1 − cos(2πi/N)).
    std::vector<float> window(n);
    for (int i = 0; i < n; ++i) window[size_t(i)] = float(std::sqrt(2.0 / 3.0) * (1 - std::cos(2 * M_PI * i / n)));
    const int frames = (int(x.size()) - n) / hop;
    const int half = n / 2;
    const int lowBins = std::max(2, int(150 / (rate / n)));
    std::vector<float> prev(size_t(half), 0), flux(size_t(frames), 0), low(size_t(frames), 0), rms(size_t(frames), 0);
    std::vector<float> frame(static_cast<size_t>(n)), mag;
    for (int f = 0; f < frames; ++f) {
        double sq = 0;
        for (int i = 0; i < n; ++i) { frame[size_t(i)] = x[size_t(f * hop + i)] * window[size_t(i)]; sq += double(frame[size_t(i)]) * frame[size_t(i)]; }
        rms[size_t(f)] = float(std::sqrt(sq / n));
        realFFTMagnitudes(frame, mag);
        float total = 0, bass = 0;
        for (int k = 1; k < half; ++k) {
            const float v = std::log1p(100 * mag[size_t(k)]);
            const float d = v - prev[size_t(k)];
            if (d > 0) { total += d; if (k < lowBins) bass += d; }
            prev[size_t(k)] = v;
        }
        flux[size_t(f)] = total;
        low[size_t(f)] = bass;
    }
    // Onset strength above its local average.
    const double fps = rate / hop;
    auto detrend = [&](const std::vector<float>& a) {
        const int r = std::max(1, int(fps * 0.25));
        std::vector<float> out(a.size(), 0), prefix(a.size() + 1, 0);
        float run = 0;
        for (size_t i = 0; i < a.size(); ++i) { run += a[i]; prefix[i + 1] = run; }
        for (size_t i = 0; i < a.size(); ++i) {
            const size_t lo = size_t(std::max(0, int(i) - r)), hi = std::min(a.size(), i + size_t(r) + 1);
            const float m = (prefix[hi] - prefix[lo]) / float(hi - lo);
            out[i] = std::max(0.0f, a[i] - m);
        }
        return out;
    };
    const auto env = detrend(flux), bassEnv = detrend(low);
    // Tempo: autocorrelation, weighted toward 120 BPM over about an octave.
    double bestBpm = 120, bestScore = -INFINITY;
    for (int bpm10 = 700; bpm10 <= 1800; bpm10 += 2) {
        const double bpm = bpm10 / 10.0;
        const double lag = 60 / bpm * fps;
        const int l0 = int(lag);
        const float frac = float(lag - l0);
        float s = 0;
        int count = 0;
        for (int i = 0; i < int(env.size()) - l0 - 1; ++i) {
            const float v = env[size_t(i + l0)] * (1 - frac) + env[size_t(i + l0 + 1)] * frac;
            s += env[size_t(i)] * v;
            ++count;
        }
        const double octave = std::log2(bpm / 120);
        const double weighted = double(s) / std::max(1, count) * std::exp(-0.5 * octave * octave / (0.9 * 0.9));
        if (weighted > bestScore) { bestScore = weighted; bestBpm = bpm; }
    }
    const double period = 60 / bestBpm * fps;
    // The beat grid's phase that lines up with the most onset strength (the bass counting as much as the rest).
    const float envTop = std::max(*std::max_element(env.begin(), env.end()), 1e-6f);
    const float bassTop = std::max(*std::max_element(bassEnv.begin(), bassEnv.end()), 1e-6f);
    std::vector<float> pulse(env.size());
    for (size_t i = 0; i < env.size(); ++i) pulse[i] = env[i] / envTop + bassEnv[i] / bassTop;
    double phase = 0;
    float phaseScore = -1;
    for (double p = 0; p < period; p += 0.5) {
        float s = 0;
        for (double t = p; int(t) < int(pulse.size()); t += period) s += pulse[size_t(int(t))];
        if (s > phaseScore) { phaseScore = s; phase = p; }
    }
    // Follow the music: each beat may move a little toward the strongest onset near where it's expected.
    std::vector<double> beatFrames;
    beatFrames.reserve(size_t(pulse.size() / std::max(1.0, period)) + 2);
    beatFrames.push_back(phase);
    double t = phase;
    while (true) {
        const double predicted = t + period;
        if (!(int(predicted) < int(pulse.size()) - 1)) break;
        const int lo = std::max(0, int(predicted - period * 0.08)), hi = std::min(int(pulse.size()) - 1, int(predicted + period * 0.08));
        double at = predicted;
        float top = 0;
        for (int i = lo; i <= hi; ++i) if (pulse[size_t(i)] > top) { top = pulse[size_t(i)]; at = i; }
        t = top > 0 ? predicted + (at - predicted) * 0.5 : predicted;
        beatFrames.push_back(t);
    }
    // Most songs keep strict time: one straight grid through the tracked beats; where a song really
    // drifts, the tracked beats, lightly smoothed.
    if (beatFrames.size() > 8) {
        const double k = double(beatFrames.size());
        const double meanI = (k - 1) / 2;
        double meanT = 0;
        for (double b : beatFrames) meanT += b;
        meanT /= k;
        double num = 0, den = 0;
        for (size_t i = 0; i < beatFrames.size(); ++i) { num += (double(i) - meanI) * (beatFrames[i] - meanT); den += (double(i) - meanI) * (double(i) - meanI); }
        const double slope = num / den, intercept = meanT - slope * meanI;
        std::vector<double> fitted(beatFrames.size());
        double res = 0;
        for (size_t i = 0; i < beatFrames.size(); ++i) { fitted[i] = intercept + slope * double(i); res += (beatFrames[i] - fitted[i]) * (beatFrames[i] - fitted[i]); }
        const double residual = std::sqrt(res / k) / fps;
        if (residual < 0.03) {
            std::vector<double> kept;
            for (double f : fitted) if (f >= 0) kept.push_back(f);
            beatFrames = std::move(kept);
        } else {
            std::vector<double> smooth(beatFrames.size());
            for (size_t i = 0; i < beatFrames.size(); ++i) {
                const size_t lo = i == 0 ? 0 : i - 1, hi = std::min(beatFrames.size() - 1, i + 1);
                double s = 0;
                for (size_t j = lo; j <= hi; ++j) s += beatFrames[j];
                smooth[i] = s / double(hi - lo + 1);
            }
            beatFrames = std::move(smooth);
        }
    }
    const double bpm = 60 / (beatFrames.size() > 1 ? (beatFrames.back() - beatFrames.front()) / double(beatFrames.size() - 1) / fps : 60 / bestBpm);
    // Bars start where the bass lands hardest.
    int barPhase = 0;
    float barScore = -1;
    for (int m = 0; m < 4; ++m) {
        float s = 0;
        for (size_t i = 0; i < beatFrames.size(); ++i) {
            if (int(i) % 4 != m) continue;
            const int k = int(std::round(beatFrames[i]));
            s += bassEnv[size_t(std::max(0, std::min(int(bassEnv.size()) - 1, k)))] + 0.25f * env[size_t(std::max(0, std::min(int(env.size()) - 1, k)))];
        }
        if (s > barScore) { barScore = s; barPhase = m; }
    }
    // Loudness of each beat.
    std::vector<float> energy;
    for (size_t i = 0; i < beatFrames.size(); ++i) {
        const int lo = int(beatFrames[i]);
        const int hi = std::min(int(rms.size()), int(i + 1 < beatFrames.size() ? beatFrames[i + 1] : beatFrames[i] + period));
        if (hi <= lo) { energy.push_back(0); continue; }
        float s = 0;
        for (int j = lo; j < hi; ++j) s += rms[size_t(j)];
        energy.push_back(s / float(hi - lo));
    }
    const float top = energy.empty() ? 1 : *std::max_element(energy.begin(), energy.end());
    if (top > 0) for (float& e : energy) e /= top;
    // Frame times are for the middle of each analysis window.
    const double offset = double(n / 2) / rate;
    MusicAnalysis m;
    m.duration = double(x.size()) / rate;
    m.bpm = bpm;
    for (double b : beatFrames) m.beats.push_back(b / fps + offset);
    m.barPhase = barPhase;
    m.energy = energy;
    return m;
}

double AutoPlanner::percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[std::min(v.size() - 1, size_t(double(v.size() - 1) * p))];
}

std::function<double(const ClipAnalysis::Sample&)> AutoPlanner::scorer(const std::vector<ClipAnalysis::Sample>& all) {
    std::vector<double> sharpLog, motion, color;
    for (const auto& s : all) { sharpLog.push_back(std::log1p(s.sharpness)); motion.push_back(s.motion); color.push_back(s.color); }
    const double s10 = percentile(sharpLog, 0.1), s90 = std::max(percentile(sharpLog, 0.9), s10 + 1e-6);
    const double m90 = std::max(percentile(motion, 0.9), 0.02);
    const double c90 = std::max(percentile(color, 0.9), 0.05);
    return [=](const ClipAnalysis::Sample& s) {
        const double sharp = std::min(1.0, std::max(0.0, (std::log1p(s.sharpness) - s10) / (s90 - s10)));
        const double exposure = 1 - std::min(1.0, std::abs(s.luma - 0.45) / 0.38);
        const double col = std::min(1.0, s.color / c90);
        const double mo = s.motion / m90;
        double q = 0.36 * sharp + 0.2 * exposure + 0.14 * col;
        q += s.faces > 0 ? 0.16 : 0;
        q += s.aesthetic ? 0.26 * (*s.aesthetic + 1) / 2 : 0.12;
        if (s.utility == true) q -= 0.35;
        if (mo > 1.1) q -= std::min(0.3, (mo - 1.1) * 0.3);
        return q;
    };
}

std::vector<AutoPlanner::Moment> AutoPlanner::moments(const ClipAnalysis& a, double limit, double length, int count,
                                                      const std::function<double(const ClipAnalysis::Sample&)>& score) {
    std::vector<double> scores;
    for (const auto& s : a.samples) scores.push_back(score(s));
    struct W { double start, value; int faces; };
    std::vector<W> windows;
    double start = 0;
    const double end = std::min(a.duration, limit);
    const double len = std::min(length, end);
    while (start + len <= end + 1e-6) {
        std::vector<double> vals;
        int faces = 0;
        for (size_t i = 0; i < a.samples.size(); ++i)
            if (a.samples[i].time >= start && a.samples[i].time <= start + len) { vals.push_back(scores[i]); faces = std::max(faces, a.samples[i].faces); }
        if (!vals.empty()) {
            double sum = 0, mn = vals[0];
            for (double v : vals) { sum += v; mn = std::min(mn, v); }
            windows.push_back({start, sum / double(vals.size()) * 0.75 + mn * 0.25, faces});
        }
        start += 0.25;
    }
    std::stable_sort(windows.begin(), windows.end(), [](const W& x, const W& y) { return x.value > y.value; });
    std::vector<Moment> picked;
    for (const auto& w : windows) {
        if (int(picked.size()) >= count) break;
        if (std::all_of(picked.begin(), picked.end(), [&](const Moment& m) { return w.start + len <= m.start || w.start >= m.end; }))
            picked.push_back({w.start, w.start + len, w.value, w.faces});
    }
    return picked;
}

} // namespace er
