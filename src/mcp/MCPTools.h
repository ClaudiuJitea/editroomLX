// MCPTools.swift: the tools agents get. Their names, titles, descriptions and schemas are the Mac's,
// read from resources/mcp-tools.json (made by scripts/extract-mcp-tools.py). Reading is instant; every
// change to the cut is one undo step.
//
// LX applies an agent's edits at once (the Mac plays them with the agent's cursor first; the cursor show
// is listed in docs/PARITY.md as not done yet). Tools that need on-device analysis (analyze_media,
// get_transcript, scene tags in search_media) answer that it isn't available yet.
#pragma once
#include "MagicEdit.h"
#include "ui/Controller.h"
#include <QJsonArray>
#include <QJsonObject>

namespace er {

struct MCPToolError { QString message; };

/// MCPDraft.swift: a Magic run's hidden copy of the project. Its agent's edits go here (each also kept,
/// to be played onto the timeline as it is when the run finishes), with its own undo.
struct MCPDraft {
    QString token;
    Project project;
    std::vector<MagicEdit> edits;
    std::vector<Project> undoStack, redoStack;
    std::vector<size_t> undoEdits, redoEdits;
    std::vector<MagicEdit> redone;
    double playhead = 0;
    int calls = 0;
    static MCPDraft* open(const Project& p);
    static MCPDraft* find(const QString& token);
    static void close(const QString& token);
    void apply(const std::vector<MagicEdit>& e);
    void undo();
    void redo();
};

namespace MCPTools {
/// The tool list and instructions, as the Mac's.
const QJsonArray& definitions();
const QString& instructions();
/// Runs a tool; returns MCP `content` (or throws MCPToolError).
QJsonArray call(const QString& name, const QJsonObject& args, Controller& c, const QString& who, MCPDraft* draft = nullptr);
/// `{"content": [...], "isError": true}`.
QJsonObject failure(const QString& message);
QString trackName(const Uuid& id, const Project& p);
QJsonObject clipJSON(const Clip& c, const Project& p);
QJsonObject projectJSON(const Project& p, const QString& title, double playhead);
/// Claude in its orange, OpenAI's agents in their green, anyone else in a colour of their own.
QColor color(const QString& name);
} // namespace MCPTools

namespace MediaNotes {
struct Note { QString text; QString by; double date = 0; };
inline constexpr int maxLength = 800;
std::optional<Note> note(const QString& path);
void save(const QString& text, const QString& path, const QString& by);
} // namespace MediaNotes

} // namespace er
