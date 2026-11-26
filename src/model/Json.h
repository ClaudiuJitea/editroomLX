// A JSON writer that produces the same bytes as Swift's JSONEncoder, so files written by LX diff
// cleanly against files written by the Mac.
//
// - `.sortedKeys` order: case-insensitive, numeric, then forced (code point) ordering, as Foundation does.
// - `.prettyPrinted`: two-space indents, `"key" : value`, and an empty array or object written as
//   "[\n\n<indent>]" (Foundation's writer has no special case for empty containers).
// - "/" escaped as "\/" (no `.withoutEscapingSlashes` on the Mac), other non-ASCII written as UTF-8.
// - Doubles in Swift's `description` form (shortest round-trip digits, exponent below 1e-4 and above
//   2^54) with a trailing ".0" dropped, which is what JSONEncoder writes: 1.0 → "1", 0.1 → "0.1".
#pragma once
#include <QJsonValue>
#include <QUuid>
#include <optional>

namespace er::json {

QByteArray write(const QJsonValue& value, bool pretty);
/// Swift `Double.description`.
QString swiftDescription(double v);
/// The number as JSONEncoder writes it.
QString formatNumber(double v);
QString quote(const QString& s);
/// Foundation's `.sortedKeys` comparison.
bool keyLess(const QString& a, const QString& b);

/// "6F1D8C64-1B2E-4C38-9A6E-0B9C1F6A1B2C", as Swift's UUID encodes.
QString uuidString(const QUuid& u);
std::optional<QUuid> parseUuid(const QString& s);

/// Equal as JSON values: same keys and values, numbers compared as doubles, and objects regardless of
/// key order. `unorderedPairs` names keys whose value is a flat [k, v, k, v…] array (a Swift dictionary
/// with non-string keys, whose order Swift doesn't keep): those compare as maps.
bool semanticallyEqual(const QJsonValue& a, const QJsonValue& b, const QStringList& unorderedPairs = {"captionFixes"});

} // namespace er::json
