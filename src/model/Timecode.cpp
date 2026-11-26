#include "Timecode.h"
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace er {

static std::atomic<TimeStyle> style{TimeStyle::timecode};
TimeStyle Timecode::currentStyle() { return style.load(); }
void Timecode::setCurrentStyle(TimeStyle s) { style.store(s); }

QString Timecode::string(double t, double fps) {
    return currentStyle() == TimeStyle::simple ? simple(t) : full(t, fps);
}

QString Timecode::simple(double t) {
    const long long cs = (long long)std::round(std::max(0.0, t) * 100);
    const long long c = cs % 100, secs = cs / 100;
    const long long s = secs % 60, m = (secs / 60) % 60, h = secs / 3600;
    return h > 0 ? QString::asprintf("%lld:%02lld:%02lld.%02lld", h, m, s, c) : QString::asprintf("%lld:%02lld.%02lld", m, s, c);
}

QString Timecode::full(double t, double fps) {
    const long long f = std::max(1LL, (long long)std::round(fps));
    const long long total = (long long)std::round(std::max(0.0, t) * fps);
    const long long frames = total % f;
    const long long secs = total / f;
    const long long s = secs % 60, m = (secs / 60) % 60, h = secs / 3600;
    return QString::asprintf("%02lld:%02lld:%02lld:%02lld", h, m, s, frames);
}

/// Swift `Int(String)`: optional sign, ASCII digits only.
static std::optional<long long> swiftInt(const QString& s) {
    if (s.isEmpty()) return std::nullopt;
    qsizetype i = 0;
    bool neg = false;
    if (s[0] == u'+' || s[0] == u'-') { neg = s[0] == u'-'; i = 1; if (s.size() == 1) return std::nullopt; }
    long long v = 0;
    for (; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c < u'0' || c > u'9') return std::nullopt;
        v = v * 10 + (c.unicode() - u'0');
        if (v > (1LL << 53)) return std::nullopt;
    }
    return neg ? -v : v;
}

std::optional<double> Timecode::parse(const QString& s, double fps) {
    const long long f = std::max(1LL, (long long)std::round(fps));
    // Swift trims `.whitespaces` (spaces and tabs, not newlines).
    QString text = s;
    while (!text.isEmpty() && (text.front().category() == QChar::Separator_Space || text.front() == u'\t')) text.remove(0, 1);
    while (!text.isEmpty() && (text.back().category() == QChar::Separator_Space || text.back() == u'\t')) text.chop(1);
    static const QRegularExpression simpleRe(QStringLiteral("^\\d+(:\\d{1,2}){0,2}(\\.\\d+)?$"));
    if (currentStyle() == TimeStyle::simple && simpleRe.match(text).hasMatch()) {
        double acc = 0;
        for (const QString& p : text.split(u':', Qt::SkipEmptyParts)) {
            bool ok = false;
            const double v = p.toDouble(&ok);
            acc = acc * 60 + (ok ? v : 0);
        }
        return acc;
    }
    std::vector<long long> parts;
    const bool separated = std::any_of(text.begin(), text.end(), [](QChar c) { return c == u':' || c == u';' || c == u'.'; });
    if (separated) {
        static const QRegularExpression sep(QStringLiteral("[:;.]"));
        const QStringList comps = text.split(sep, Qt::SkipEmptyParts);
        if (comps.isEmpty() || comps.size() > 4) return std::nullopt;
        for (const QString& c : comps) {
            auto v = swiftInt(c);
            if (!v || *v < 0) return std::nullopt;
            parts.push_back(*v);
        }
    } else {
        if (text.isEmpty() || text.size() > 8) return std::nullopt;
        // Swift's `isNumber` also accepts non-ASCII digits; those then read as 0 via Int(), as here.
        if (!std::all_of(text.begin(), text.end(), [](QChar c) { return c.isNumber(); })) return std::nullopt;
        QString digits = text;
        while (!digits.isEmpty()) {
            const QString pair = digits.right(2);
            parts.insert(parts.begin(), swiftInt(pair).value_or(0));
            digits.chop(pair.size());
        }
    }
    std::vector<long long> r(parts.rbegin(), parts.rend());
    for (int i = 0; i < 4; ++i) r.push_back(0);
    const long long frames = ((r[3] * 60 + r[2]) * 60 + r[1]) * f + r[0];
    return double(frames) / fps;
}

QString Timecode::shortString(double t) {
    const long long secs = (long long)std::round(std::max(0.0, t));
    const long long s = secs % 60, m = (secs / 60) % 60, h = secs / 3600;
    return h > 0 ? QString::asprintf("%lld:%02lld:%02lld", h, m, s) : QString::asprintf("%lld:%02lld", m, s);
}

} // namespace er
