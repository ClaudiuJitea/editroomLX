#include "Json.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace er::json {

QString swiftDescription(double v) {
    if (std::isnan(v)) return QStringLiteral("nan");
    if (std::isinf(v)) return v < 0 ? QStringLiteral("-inf") : QStringLiteral("inf");
    if (v == 0) return std::signbit(v) ? QStringLiteral("-0.0") : QStringLiteral("0.0");
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    // "-d.ddde±XX"
    bool neg = false;
    if (sci[0] == '-') { neg = true; sci.erase(0, 1); }
    const auto e = sci.find('e');
    std::string mant = sci.substr(0, e);
    const int exp10 = std::stoi(sci.substr(e + 1));
    std::string digits;
    for (char c : mant) if (c != '.') digits += c;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    // swift_dtoa: exponential when the decimal exponent (0.ddd × 10^k form) is below -3 or |v| > 2^54.
    const int decimalExponent = exp10 + 1;
    if (decimalExponent < -3 || std::fabs(v) > 0x1.0p54) {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += 'e';
        out += exp10 < 0 ? '-' : '+';
        const int ae = std::abs(exp10);
        if (ae < 10) out += '0';
        out += std::to_string(ae);
    } else if (exp10 >= 0) {
        const size_t intLen = size_t(exp10) + 1;
        if (digits.size() <= intLen) {
            out = digits + std::string(intLen - digits.size(), '0') + ".0";
        } else {
            out = digits.substr(0, intLen) + "." + digits.substr(intLen);
        }
    } else {
        out = "0." + std::string(size_t(-exp10 - 1), '0') + digits;
    }
    if (neg) out.insert(out.begin(), '-');
    return QString::fromLatin1(out.c_str());
}

QString formatNumber(double v) {
    QString s = swiftDescription(v);
    if (s.endsWith(QLatin1String(".0"))) s.chop(2);
    return s;
}

QString quote(const QString& s) {
    QString out;
    out.reserve(s.size() + 2);
    out += u'"';
    for (QChar c : s) {
        switch (c.unicode()) {
        case '"': out += QLatin1String("\\\""); break;
        case '\\': out += QLatin1String("\\\\"); break;
        case '/': out += QLatin1String("\\/"); break;
        case '\b': out += QLatin1String("\\b"); break;
        case '\f': out += QLatin1String("\\f"); break;
        case '\n': out += QLatin1String("\\n"); break;
        case '\r': out += QLatin1String("\\r"); break;
        case '\t': out += QLatin1String("\\t"); break;
        default:
            if (c.unicode() < 0x20) out += QString::asprintf("\\u%04x", c.unicode());
            else out += c;
        }
    }
    out += u'"';
    return out;
}

bool keyLess(const QString& a, const QString& b) {
    // [.numeric, .caseInsensitive, .forcedOrdering]
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            qsizetype i2 = i, j2 = j;
            while (i2 < a.size() && a[i2].isDigit()) ++i2;
            while (j2 < b.size() && b[j2].isDigit()) ++j2;
            QString na = a.mid(i, i2 - i), nb = b.mid(j, j2 - j);
            while (na.size() > 1 && na[0] == u'0') na.remove(0, 1);
            while (nb.size() > 1 && nb[0] == u'0') nb.remove(0, 1);
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = i2; j = j2;
            continue;
        }
        const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
        if (ca != cb) return ca < cb;
        ++i; ++j;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b; // forced ordering
}

static void writeValue(const QJsonValue& v, bool pretty, int depth, QString& out);

static void indent(QString& out, int depth) { out += QString(depth * 2, u' '); }

static void writeValue(const QJsonValue& v, bool pretty, int depth, QString& out) {
    switch (v.type()) {
    case QJsonValue::Null: out += QLatin1String("null"); break;
    case QJsonValue::Bool: out += v.toBool() ? QLatin1String("true") : QLatin1String("false"); break;
    case QJsonValue::Double: out += formatNumber(v.toDouble()); break;
    case QJsonValue::String: out += quote(v.toString()); break;
    case QJsonValue::Array: {
        const QJsonArray a = v.toArray();
        out += u'[';
        if (pretty) out += u'\n';
        bool first = true;
        for (const auto& e : a) {
            if (!first) out += pretty ? QLatin1String(",\n") : QLatin1String(",");
            first = false;
            if (pretty) indent(out, depth + 1);
            writeValue(e, pretty, depth + 1, out);
        }
        if (pretty) { out += u'\n'; indent(out, depth); }
        out += u']';
        break;
    }
    case QJsonValue::Object: {
        const QJsonObject o = v.toObject();
        QStringList keys = o.keys();
        std::sort(keys.begin(), keys.end(), keyLess);
        out += u'{';
        if (pretty) out += u'\n';
        bool first = true;
        for (const auto& k : keys) {
            if (!first) out += pretty ? QLatin1String(",\n") : QLatin1String(",");
            first = false;
            if (pretty) indent(out, depth + 1);
            out += quote(k);
            out += pretty ? QLatin1String(" : ") : QLatin1String(":");
            writeValue(o.value(k), pretty, depth + 1, out);
        }
        if (pretty) { out += u'\n'; indent(out, depth); }
        out += u'}';
        break;
    }
    case QJsonValue::Undefined: break;
    }
}

QByteArray write(const QJsonValue& value, bool pretty) {
    QString out;
    writeValue(value, pretty, 0, out);
    return out.toUtf8();
}

QString uuidString(const QUuid& u) { return u.toString(QUuid::WithoutBraces).toUpper(); }

std::optional<QUuid> parseUuid(const QString& s) {
    // Swift's UUID(uuidString:) wants exactly the 36-character hyphenated form (any case).
    if (s.size() != 36) return std::nullopt;
    const QUuid u = QUuid::fromString(s);
    if (u.isNull() && s != QLatin1String("00000000-0000-0000-0000-000000000000")) return std::nullopt;
    return u;
}

static bool pairsEqual(const QJsonArray& a, const QJsonArray& b, const QStringList& keys) {
    if (a.size() != b.size() || a.size() % 2) return false;
    std::map<QString, QJsonValue> ma, mb;
    for (qsizetype i = 0; i + 1 < a.size(); i += 2) ma[a[i].toString().toUpper()] = a[i + 1];
    for (qsizetype i = 0; i + 1 < b.size(); i += 2) mb[b[i].toString().toUpper()] = b[i + 1];
    if (ma.size() != mb.size()) return false;
    for (const auto& [k, v] : ma) {
        auto it = mb.find(k);
        if (it == mb.end() || !semanticallyEqual(v, it->second, keys)) return false;
    }
    return true;
}

bool semanticallyEqual(const QJsonValue& a, const QJsonValue& b, const QStringList& keys) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case QJsonValue::Double: return a.toDouble() == b.toDouble();
    case QJsonValue::Array: {
        const QJsonArray x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) return false;
        for (qsizetype i = 0; i < x.size(); ++i) if (!semanticallyEqual(x[i], y[i], keys)) return false;
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject x = a.toObject(), y = b.toObject();
        if (x.size() != y.size()) return false;
        for (auto it = x.begin(); it != x.end(); ++it) {
            if (!y.contains(it.key())) return false;
            const QJsonValue yv = y.value(it.key());
            if (keys.contains(it.key()) && it.value().isArray() && yv.isArray()) {
                if (!pairsEqual(it.value().toArray(), yv.toArray(), keys)) return false;
            } else if (!semanticallyEqual(it.value(), yv, keys)) {
                return false;
            }
        }
        return true;
    }
    default: return a == b;
    }
}

} // namespace er::json
