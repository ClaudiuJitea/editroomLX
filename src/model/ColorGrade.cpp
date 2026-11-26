#include "ColorGrade.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QRegularExpression>
#include <cmath>
#include <map>
#include <mutex>

namespace er {

bool ColorGrade::sameGrade(const ColorGrade& o) const {
    return look == o.look && lookIntensity == o.lookIntensity && exposure == o.exposure && contrast == o.contrast &&
           saturation == o.saturation && temperature == o.temperature && tint == o.tint &&
           shadows.x == o.shadows.x && shadows.y == o.shadows.y && midtones.x == o.midtones.x &&
           midtones.y == o.midtones.y && highlights.x == o.highlights.x && highlights.y == o.highlights.y;
}

bool ColorGrade::isNeutral() const {
    ColorGrade g = *this;
    if (!g.look) g.lookIntensity = ColorGrade().lookIntensity;
    return g.sameGrade(ColorGrade());
}

QString ColorGrade::lookLabel() const {
    if (!look) return QStringLiteral("None");
    if (BuiltInLook::named(*look)) return *look;
    return QFileInfo(*look).completeBaseName().isEmpty() ? QFileInfo(*look).fileName() : QFileInfo(*look).completeBaseName();
}

RGB WheelPoint::offset() const {
    const double m = std::min(1.0, std::sqrt(x * x + y * y)) * 0.22;
    if (!(m > 0)) return {0, 0, 0};
    const double a = std::atan2(y, x);
    const double third = 2 * M_PI / 3;
    return {std::cos(a) * m, std::cos(a - third) * m, std::cos(a + third) * m};
}

RGB GradeRecipe::apply(RGB rgb) const {
    if (custom) return custom(rgb);
    double r = rgb.r, g = rgb.g, b = rgb.b;
    const double gain = std::pow(2.0, exposure);
    r *= gain; g *= gain; b *= gain;
    const double t = temperature / 50 * 0.12, ti = tint / 50 * 0.1;
    r += t; b -= t; g -= ti;
    // Tonal weights from luma: shadows fall off toward white, highlights toward black.
    const double l = std::min(1.0, std::max(0.0, 0.2126 * r + 0.7152 * g + 0.0722 * b));
    const double sh = 1 - l, mid = 4 * l * (1 - l);
    r += shadows.r * sh + midtones.r * mid + highlights.r * l + lift.r;
    g += shadows.g * sh + midtones.g * mid + highlights.g * l + lift.g;
    b += shadows.b * sh + midtones.b * mid + highlights.b * l + lift.b;
    const double k = 1 + contrast / 50 * 0.6;
    r = (r - 0.5) * k + 0.5; g = (g - 0.5) * k + 0.5; b = (b - 0.5) * k + 0.5;
    if (fade != 0) {
        r = r * (1 - fade) + fade; g = g * (1 - fade) + fade; b = b * (1 - fade) + fade;
    }
    const double l2 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double s = 1 + saturation / 100;
    return {l2 + (r - l2) * s, l2 + (g - l2) * s, l2 + (b - l2) * s};
}

// MARK: LookMath

namespace {
using std::max;
using std::min;
double clamp(double x) { return min(1.0, max(0.0, x)); }
double luma(RGB c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
template <class F> RGB each(RGB c, F f) { return {f(c.r), f(c.g), f(c.b)}; }
/// Positive adds an S-curve; negative flattens.
RGB contrastS(RGB c, double k) {
    return each(c, [&](double x) { x = clamp(x); const double s = x * x * (3 - 2 * x); return clamp(x + k * (s - x) * 2); });
}
RGB fadeTo(RGB c, double f) { return each(c, [&](double v) { return f + (1 - f) * v; }); }
RGB saturate(RGB c, double s) { const double l = luma(c); return each(c, [&](double v) { return clamp(l + s * (v - l)); }); }
RGB add(RGB c, RGB t, double w = 1) { return {clamp(c.r + t.r * w), clamp(c.g + t.g * w), clamp(c.b + t.b * w)}; }
/// Tints that land mostly in the shadows and mostly in the highlights.
RGB splitTone(RGB c, RGB shadows, RGB highlights) {
    const double l = clamp(luma(c));
    return add(add(c, shadows, (1 - l) * (1 - l)), highlights, l * l);
}
struct HSV { double h, s, v; };
HSV toHSV(RGB c) {
    const double mx = max({c.r, c.g, c.b}), mn = min({c.r, c.g, c.b}), d = mx - mn;
    double h = 0.0;
    if (d > 1e-6) {
        if (mx == c.r) h = 60 * std::fmod((c.g - c.b) / d, 6.0);
        else if (mx == c.g) h = 60 * ((c.b - c.r) / d + 2);
        else h = 60 * ((c.r - c.g) / d + 4);
    }
    if (h < 0) h += 360;
    return {h, mx > 0 ? d / mx : 0, mx};
}
RGB fromHSV(double h, double s, double v) {
    const double c = v * s, x = c * (1 - std::abs(std::fmod(h / 60, 2.0) - 1)), m = v - c;
    if (h < 60) return {c + m, x + m, m};
    if (h < 120) return {x + m, c + m, m};
    if (h < 180) return {m, c + m, x + m};
    if (h < 240) return {m, x + m, c + m};
    if (h < 300) return {x + m, m, c + m};
    return {c + m, m, x + m};
}
/// Shifts the hue and scales the saturation (and brightness) of colours near `center`, fading
/// out over `width` degrees and leaving greys and near-whites alone.
RGB hueBand(RGB c, double center, double width, double shift, double sat, double value = 1) {
    auto [h, s, v] = toHSV(c);
    double d = std::abs(h - center);
    if (d > 180) d = 360 - d;
    if (!(d < width)) return c;
    const double w = (0.5 + 0.5 * std::cos(d / width * M_PI)) * min(1.0, s * 2.5);
    h = std::fmod(h + shift * w + 360, 360.0);
    s = clamp(s * (1 + (sat - 1) * w));
    v = clamp(v * (1 + (value - 1) * w));
    return fromHSV(h, s, v);
}
/// Warmth (r, g, b), gamma, skin hue/saturation, green hue/saturation, blue hue/saturation,
/// shadow tint (r, g, b), highlight tint (r, g, b), contrast, saturation, red saturation and brightness.
constexpr double filmPrintSettings[20] = {
    0.1260, 0.0872, 0.1130, 0.9273, 4.3723, 1.1754, -6.7912, 0.8993, -3.0058, 1.0943,
    -0.2338, -0.1982, -0.2908, -0.1016, -0.0275, -0.0710, 0.1846, 0.8248, 1.0324, 1.0304,
};
} // namespace

/// The warm film-print look, inspired by Kodak 2383 print film: golden skin, dense rich reds,
/// teal-leaning shadows, filmic contrast, and a touch of fade for soft blacks. Our own maths,
/// with its settings tuned until it behaves like that print film on test footage.
RGB LookMath::filmPrint(RGB c0) {
    const double* p = filmPrintSettings;
    RGB c = add(each(c0, clamp), {p[0], p[1], p[2]});
    c = each(c, [&](double v) { return std::pow(clamp(v), p[3]); });
    c = hueBand(c, 28, 22, p[4], p[5]);
    c = hueBand(c, 110, 60, p[6], p[7]);
    c = hueBand(c, 210, 45, p[8], p[9]);
    c = splitTone(c, {p[10], p[11], p[12]}, {p[13], p[14], p[15]});
    c = contrastS(c, p[16]);
    c = saturate(c, p[17]);
    c = hueBand(c, 356, 20, 0, p[18], p[19]);
    return fadeTo(c, 0.035);
}

/// Amber highlights, slightly cool shadows: late-afternoon light.
RGB LookMath::goldenHour(RGB c) {
    c = splitTone(c, {-0.012, 0, 0.03}, {0.085, 0.04, -0.06});
    c = contrastS(c, 0.08);
    return saturate(c, 1.08);
}

/// High contrast and very little colour.
RGB LookMath::bleachBypass(RGB c) {
    c = saturate(each(c, clamp), 0.42);
    c = contrastS(c, 0.42);
    return add(c, {-0.01, 0, 0.008});
}

/// Lifted shadows, soft bright colours.
RGB LookMath::pastel(RGB c) {
    c = each(c, [](double v) { return std::pow(clamp(v), 0.8); });
    c = contrastS(c, -0.36);
    c = fadeTo(c, 0.14);
    c = splitTone(c, {0.03, 0, 0.05}, {0.01, 0, 0.012});
    return saturate(c, 0.8);
}

// MARK: BuiltInLook

namespace {
GradeRecipe recipe(double contrast, double saturation, double temperature = 0, double exposure = 0) {
    GradeRecipe r; r.contrast = contrast; r.saturation = saturation; r.temperature = temperature; r.exposure = exposure;
    return r;
}
GradeRecipe customRecipe(RGB (*f)(RGB)) { GradeRecipe r; r.custom = f; return r; }
}

const std::vector<std::pair<QString, GradeRecipe>>& BuiltInLook::all() {
    static const std::vector<std::pair<QString, GradeRecipe>> looks = [] {
        GradeRecipe warm = recipe(8, -8, 18); warm.lift = {0.02, 0, -0.02}; warm.fade = 0.03;
        GradeRecipe teal = recipe(12, 10); teal.shadows = {-0.05, 0.01, 0.05}; teal.highlights = {0.06, 0.01, -0.05};
        return std::vector<std::pair<QString, GradeRecipe>>{
            {QStringLiteral("Warm film"), warm},
            {QStringLiteral("Golden hour"), customRecipe(LookMath::goldenHour)},
            {QStringLiteral("Film print"), customRecipe(LookMath::filmPrint)},
            {QStringLiteral("Teal and orange"), teal},
            {QStringLiteral("Pastel"), customRecipe(LookMath::pastel)},
            {QStringLiteral("Bleach bypass"), customRecipe(LookMath::bleachBypass)},
            {QStringLiteral("Mono"), recipe(14, -100)},
            {QStringLiteral("Cool night"), recipe(6, -15, -22, -0.25)},
        };
    }();
    return looks;
}

std::optional<GradeRecipe> BuiltInLook::named(const QString& name) {
    for (const auto& [n, r] : all()) if (n == name) return r;
    // No longer offered, but still read, so projects that used them look the same.
    if (name == QStringLiteral("Faded")) { GradeRecipe r = recipe(-18, -20); r.fade = 0.07; return r; }
    return std::nullopt;
}

// MARK: CubeLUT

std::shared_ptr<CubeLUT> CubeLUT::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    return parse(QString::fromUtf8(f.readAll()));
}

std::shared_ptr<CubeLUT> CubeLUT::parse(const QString& text) {
    int size = 0;
    std::vector<float> values;
    // Swift splits on any newline character and drops empty lines.
    for (const QString& raw : text.split(QRegularExpression(QStringLiteral("[\\n\\r\\x0B\\x0C\\x{85}\\x{2028}\\x{2029}]")), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) continue;
        const QStringList parts = line.split(u' ', Qt::SkipEmptyParts);
        if (parts[0] == QStringLiteral("LUT_3D_SIZE") && parts.size() > 1) {
            bool ok = false;
            size = parts[1].toInt(&ok);
            if (!ok) size = 0;
            if (size > 0 && size <= 256) values.reserve(size_t(size) * size * size * 3);
        } else if (parts.size() == 3) {
            bool a = false, b = false, c = false;
            const float r = parts[0].toFloat(&a), g = parts[1].toFloat(&b), bl = parts[2].toFloat(&c);
            if (a && b && c) { values.push_back(r); values.push_back(g); values.push_back(bl); }
        }
    }
    if (size < 2 || size > 256 || values.size() != size_t(size) * size * size * 3) return nullptr;
    auto lut = std::make_shared<CubeLUT>();
    lut->size_ = size;
    lut->values_ = std::move(values);
    return lut;
}

RGB CubeLUT::sample(RGB rgb) const {
    const double n = double(size_ - 1);
    struct S { int i0, i1; double f; };
    auto split = [&](double v) {
        const double p = std::min(1.0, std::max(0.0, v)) * n;
        const int i = std::min(size_ - 2, int(p));
        return S{i, i + 1, p - double(i)};
    };
    const S R = split(rgb.r), G = split(rgb.g), B = split(rgb.b);
    auto at = [&](int r, int g, int b, int c) { return double(values_[((size_t(b) * size_ + g) * size_ + r) * 3 + c]); };
    double out[3];
    for (int c = 0; c < 3; ++c) {
        const double c00 = at(R.i0, G.i0, B.i0, c) * (1 - R.f) + at(R.i1, G.i0, B.i0, c) * R.f;
        const double c10 = at(R.i0, G.i1, B.i0, c) * (1 - R.f) + at(R.i1, G.i1, B.i0, c) * R.f;
        const double c01 = at(R.i0, G.i0, B.i1, c) * (1 - R.f) + at(R.i1, G.i0, B.i1, c) * R.f;
        const double c11 = at(R.i0, G.i1, B.i1, c) * (1 - R.f) + at(R.i1, G.i1, B.i1, c) * R.f;
        out[c] = (c00 * (1 - G.f) + c10 * G.f) * (1 - B.f) + (c01 * (1 - G.f) + c11 * G.f) * B.f;
    }
    return {out[0], out[1], out[2]};
}

// MARK: GradeBaker

namespace {
std::mutex bakerLock;
struct GradeKey {
    ColorGrade g;
    bool operator<(const GradeKey& o) const {
        auto tie = [](const ColorGrade& c) {
            return std::make_tuple(c.look.value_or(QString()), c.look.has_value(), c.lookIntensity, c.exposure, c.contrast,
                                   c.saturation, c.temperature, c.tint, c.shadows.x, c.shadows.y, c.midtones.x,
                                   c.midtones.y, c.highlights.x, c.highlights.y);
        };
        return tie(g) < tie(o.g);
    }
};
std::map<GradeKey, std::shared_ptr<const std::vector<float>>> cache;
QHash<QString, std::shared_ptr<CubeLUT>> cubes;
} // namespace

std::shared_ptr<CubeLUT> GradeBaker::cubeFile(const QString& path) {
    std::lock_guard lock(bakerLock);
    if (auto hit = cubes.value(path)) return hit;
    auto lut = CubeLUT::load(path);
    if (!lut) return nullptr;
    cubes.insert(path, lut);
    return lut;
}

std::shared_ptr<const std::vector<float>> GradeBaker::cube(const std::optional<ColorGrade>& gradeIn) {
    if (!gradeIn || gradeIn->isNeutral()) return nullptr;
    ColorGrade grade = *gradeIn;
    grade.extra = {};
    {
        std::lock_guard lock(bakerLock);
        if (auto it = cache.find({grade}); it != cache.end()) return it->second;
    }
    std::optional<GradeRecipe> lookRecipe;
    std::shared_ptr<CubeLUT> lookFile;
    if (grade.look) {
        lookRecipe = BuiltInLook::named(*grade.look);
        if (!lookRecipe) lookFile = cubeFile(*grade.look);
    }
    GradeRecipe user;
    user.exposure = grade.exposure; user.contrast = grade.contrast; user.saturation = grade.saturation;
    user.temperature = grade.temperature; user.tint = grade.tint;
    user.shadows = grade.shadows.offset(); user.midtones = grade.midtones.offset(); user.highlights = grade.highlights.offset();
    const double mix = std::min(1.0, std::max(0.0, grade.lookIntensity));
    const int n = dimension;
    auto data = std::make_shared<std::vector<float>>(size_t(n) * n * n * 4, 1.0f);
    float* out = data->data();
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) {
                RGB c{double(r) / double(n - 1), double(g) / double(n - 1), double(b) / double(n - 1)};
                if (mix > 0 && (lookRecipe || lookFile)) {
                    const RGB looked = lookRecipe ? lookRecipe->apply(c) : lookFile->sample(c);
                    c = {c.r + (looked.r - c.r) * mix, c.g + (looked.g - c.g) * mix, c.b + (looked.b - c.b) * mix};
                }
                c = user.apply(c);
                const size_t i = ((size_t(b) * n + g) * n + r) * 4;
                out[i] = float(std::min(1.0, std::max(0.0, c.r)));
                out[i + 1] = float(std::min(1.0, std::max(0.0, c.g)));
                out[i + 2] = float(std::min(1.0, std::max(0.0, c.b)));
            }
        }
    }
    std::lock_guard lock(bakerLock);
    if (cache.size() > 200) cache.clear();
    cache[{grade}] = data;
    return data;
}

} // namespace er
