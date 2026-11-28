// The `.room` file format: Codable for every model type, matching Swift's synthesised Codable (and the
// Mac's hand-written decoders for Track and CaptionStyle). Unknown keys are kept and written back.
#pragma once
#include "Project.h"
#include <QJsonObject>

namespace er {

/// `SavedDocument {version: 1, project}` (EditorModel.swift).
struct SavedDocument {
    int version = 1;
    Project project;
    Extra extra;
    bool operator==(const SavedDocument&) const = default;
};

namespace codec {

struct Result {
    std::optional<SavedDocument> document;
    QString error;
};

QJsonObject encode(const Project& p);
QJsonObject encode(const SavedDocument& d);
/// Throws nothing; on failure `error` names the key path, like Swift's DecodingError.
Result decodeDocument(const QByteArray& bytes);
std::optional<Project> decodeProject(const QJsonObject& o, QString* error = nullptr);

/// A whole `.room` file as the Mac's Save writes it: pretty-printed, sorted keys.
QByteArray saveBytes(const SavedDocument& d);
/// As the Mac's autosave and recovery write it (compact). LX sorts keys here too, for stable diffs.
QByteArray compactBytes(const SavedDocument& d);

/// One grade, for tests (the Mac's GRADE LEVELS check reads a grade from before its levels existed).
QJsonObject encode(const ColorGrade& g);
ColorGrade decodeGrade(const QJsonObject& o);

QJsonObject encode(const Transcript& t);
std::optional<Transcript> decodeTranscript(const QJsonObject& o);

} // namespace codec
} // namespace er
