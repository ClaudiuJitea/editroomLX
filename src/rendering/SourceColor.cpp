#include "SourceColor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace er {

namespace {
using Matrix = std::array<std::array<double, 3>, 3>;
using XY = std::pair<double, double>;

constexpr XY d65{0.3127, 0.3290};
const std::array<XY, 3> rec709Primaries{{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}};
const std::array<XY, 3> rec2020{{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}};
/// Apple Wide Gamut, from Apple's Apple Log 2 white paper (blue's y really is negative).
const std::array<XY, 3> appleWideGamut{{{0.725, 0.301}, {0.221, 0.814}, {0.068, -0.076}}};

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a[r][k] * b[k][c];
            m[r][c] = s;
        }
    return m;
}

Matrix inverse(const Matrix& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return {{{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
             {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
             {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}}};
}

Matrix toXYZ(const std::array<XY, 3>& p) {
    std::array<std::array<double, 3>, 3> cols;
    for (int i = 0; i < 3; ++i) cols[i] = {p[i].first / p[i].second, 1, (1 - p[i].first - p[i].second) / p[i].second};
    Matrix m;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m[r][c] = cols[c][r];
    const std::array<double, 3> w{d65.first / d65.second, 1, (1 - d65.first - d65.second) / d65.second};
    const Matrix inv = inverse(m);
    std::array<double, 3> s;
    for (int r = 0; r < 3; ++r) s[r] = inv[r][0] * w[0] + inv[r][1] * w[1] + inv[r][2] * w[2];
    Matrix out;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) out[r][c] = m[r][c] * s[c];
    return out;
}

/// Linear RGB in one set of primaries to another. Both share the D65 white point, so no white
/// adaptation is needed.
Matrix convertPrimaries(const std::array<XY, 3>& a, const std::array<XY, 3>& b) { return multiply(inverse(toXYZ(b)), toXYZ(a)); }

const Matrix& rec2020To709() { static const Matrix m = convertPrimaries(rec2020, rec709Primaries); return m; }
const Matrix& wideGamutTo709() { static const Matrix m = convertPrimaries(appleWideGamut, rec709Primaries); return m; }

constexpr double knee = 0.9;
/// Where HDR's reference white lands in SDR linear light, and where its highlights start easing.
constexpr double hdrGain = 0.317;
constexpr double hdrPower = 0.88;
constexpr double hdrLift = 0.0143;
constexpr double hdrKnee = 0.85;
constexpr double hdrSaturation = 0.88;

double decode(SourceColor kind, double v) {
    if (kind == SourceColor::hlgVision) kind = SourceColor::hlg;
    if (kind == SourceColor::pqVision) kind = SourceColor::pq;
    switch (kind) {
    case SourceColor::standard: return v;
    case SourceColor::hlg: {
        // Inverse HLG curve; HLG puts diffuse white at 75% (scene light 0.2647, BT.2408).
        const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
        const double e = v <= 0.5 ? v * v / 3 : (std::exp((v - c) / a) + b) / 12;
        return e / 0.2647;
    }
    case SourceColor::pq: {
        // PQ to nits, with 203 nits as white (BT.2408).
        const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const double p = std::pow(std::max(0.0, v), 1 / m2);
        const double y = std::pow(std::max(0.0, p - c1) / (c2 - c3 * p), 1 / m1);
        return y * 10000 / 203;
    }
    case SourceColor::hlgVision:
    case SourceColor::pqVision:
        break;
    case SourceColor::appleLog:
    case SourceColor::appleLog2: {
        // Apple Log (and Log 2, which shares the curve) to scene light (18% grey = 0.18), from Apple's published curve.
        const double R0 = -0.05641088, Rt = 0.01, c = 47.28711236, b = 0.00964052, g = 0.08550479, be = 0.69336945;
        const double Pt = c * (Rt - R0) * (Rt - R0);
        if (v >= Pt) return std::pow(2.0, (v - be) / g) - b;
        if (v > 0) return std::sqrt(v / c) + R0;
        return R0;
    }
    }
    return v;
}

double rec709(double x) {
    const double v = std::min(1.0, std::max(0.0, x));
    return v < 0.018 ? 4.5 * v : 1.099 * std::pow(v, 0.45) - 0.099;
}
} // namespace

std::tuple<double, double, double> SourceColors::convert(SourceColor kind, double r, double g, double b) {
    // Linear light, scaled so the format's reference white is 1.
    const double lin[3] = {decode(kind, r), decode(kind, g), decode(kind, b)};
    // The file's primaries to Rec.709.
    const Matrix& m = kind == SourceColor::appleLog2 ? wideGamutTo709() : rec2020To709();
    double c[3];
    for (int i = 0; i < 3; ++i) c[i] = std::max(0.0, m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2]);
    // iPhone HDR (HLG or PQ with Dolby Vision) is compressed into SDR the way macOS does it: 0.317 × light^0.88 + 0.014,
    // fitted to macOS's own frames of iPhone HLG (within about 2% from shadows to highlights; the small lift fades out
    // toward black, so black stays black). The far greater range above white (sun, sky, lit skin) has room instead of
    // being squeezed into the last few percent, which made outdoor iPhone video look blown out. The Mac now has
    // AVFoundation convert HDR as it's read, steered by the Dolby Vision metadata; Linux has no such engine, so this
    // curve, fitted to its output, is the nearest match. HDR without Dolby Vision (BT.2408 graded, as the test patches)
    // is converted by AVFoundation with white at white, which the plain mapping below already does.
    const bool hdr = kind == SourceColor::hlgVision || kind == SourceColor::pqVision;
    if (hdr) {
        const double y = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        if (y > 1e-6) {
            const double target = hdrGain * std::pow(y, hdrPower) + hdrLift * std::min(1.0, y / 0.02);
            const double scale = target / y;
            for (double& x : c) x *= scale;
            // Darkened, colours read richer than they did; eased a little toward grey, as macOS's conversion is.
            const double g = y * scale;
            for (double& x : c) x = g + (x - g) * hdrSaturation;
        }
    }
    // Brighter than white rolls off smoothly instead of clipping; scaling all three channels together keeps the
    // hue. HDR starts easing sooner, with more to fit in.
    const double k = hdr ? hdrKnee : knee;
    const double peak = std::max({c[0], c[1], c[2]});
    if (peak > k) {
        const double eased = k + (1 - k) * (1 - std::exp(-(peak - k) / (1 - k)));
        for (double& x : c) x = x * eased / peak;
    }
    return {rec709(c[0]), rec709(c[1]), rec709(c[2])};
}

std::shared_ptr<const std::vector<float>> SourceColors::cube(SourceColor kind) {
    if (kind == SourceColor::standard) return nullptr;
    static std::mutex lock;
    static std::shared_ptr<const std::vector<float>> cubes[7];
    std::lock_guard g(lock);
    auto& slot = cubes[int(kind)];
    if (slot) return slot;
    const int n = dimension;
    auto values = std::make_shared<std::vector<float>>(size_t(n) * n * n * 4, 1.0f);
    for (int b = 0; b < n; ++b)
        for (int gg = 0; gg < n; ++gg)
            for (int r = 0; r < n; ++r) {
                auto [x, y, z] = convert(kind, double(r) / (n - 1), double(gg) / (n - 1), double(b) / (n - 1));
                const size_t i = ((size_t(b) * n + gg) * n + r) * 4;
                (*values)[i] = float(x); (*values)[i + 1] = float(y); (*values)[i + 2] = float(z);
            }
    slot = values;
    return slot;
}

const char* SourceColors::name(SourceColor kind) {
    switch (kind) {
    case SourceColor::standard: return "standard";
    case SourceColor::hlg: return "hlg";
    case SourceColor::pq: return "pq";
    case SourceColor::appleLog: return "appleLog";
    case SourceColor::appleLog2: return "appleLog2";
    case SourceColor::hlgVision: return "hlgVision";
    case SourceColor::pqVision: return "pqVision";
    }
    return "?";
}

} // namespace er
