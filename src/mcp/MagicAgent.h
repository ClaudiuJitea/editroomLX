// MagicAgent.swift and AgentConnect.swift: Magic with Claude (Claude Code) or ChatGPT (Codex). The agent
// runs with only Editroom's tools (and web search, to find music), pointed at this copy's MCP server with
// a `?draft=` token, so its edits go to a hidden draft; when it finishes they land on the timeline as one
// undo step, and what it said is shown. AgentConnect adds Editroom to Claude Code (`claude mcp add`) and
// to Codex (`~/.codex/config.toml`) for people's own sessions.
#pragma once
#include "MagicMentions.h"
#include "MCPServer.h"
#include "MCPTools.h"
#include <QProcess>

namespace er {

class MagicAgent : public QObject {
    Q_OBJECT
public:
    enum class Kind { claude, chatgpt };
    struct ModelOption { QString id, name, blurb; QStringList efforts; QString defaultEffort; };
    MagicAgent(Controller& c, MCPServer& server, QObject* parent = nullptr);

    static QString name(Kind k) { return k == Kind::claude ? QStringLiteral("Claude") : QStringLiteral("ChatGPT"); }
    static QString rawValue(Kind k) { return k == Kind::claude ? QStringLiteral("claude") : QStringLiteral("chatgpt"); }
    /// The CLI on $PATH, ~/.local/bin or ~/.npm-global/bin (nullopt: not installed).
    static QString cli(Kind k);
    std::vector<ModelOption> models(Kind k) const;
    QString model(Kind k) const;
    QString effort(Kind k) const;
    void setModel(Kind k, const QString& id, const QString& effort);

    static QString brief(const QString& request, const Project& p, const std::optional<MagicPointers>& pointers = std::nullopt);
    /// Where they were pointing when they asked, and what each @ in the request means (MagicAgent.pointing).
    static std::optional<QString> pointing(const MagicPointers& ptr, const QString& request, const Project& p);
    void start(const QString& request, Kind k);
    void stop();
    bool running() const { return process_ != nullptr; }
    QString status() const { return status_; }

signals:
    void statusChanged(const QString& status);
    /// The run ended: what the agent said, and whether the timeline changed.
    void finished(const QString& said, bool changed, const QString& failure);

private:
    void read(const QByteArray& line);
    void done(int code);
    static QString doing(const QString& tool);
    Controller& c_;
    MCPServer& server_;
    QProcess* process_ = nullptr;
    Kind kind_ = Kind::claude;
    MCPDraft* draft_ = nullptr;
    QByteArray pending_, errors_;
    QString lastMessage_, failure_, status_;
    bool stopped_ = false;
};

namespace AgentConnect {
/// `claude mcp add --scope user --transport http editroom <endpoint>`.
bool connectClaudeCode(QString* error = nullptr);
bool disconnectClaudeCode(QString* error = nullptr);
bool claudeCodeConnected();
/// An `[mcp_servers.editroom]` section in ~/.codex/config.toml (a copy of the old file kept once).
bool connectCodex(QString* error = nullptr);
bool disconnectCodex(QString* error = nullptr);
bool codexConnected();
QString removingEditroom(const QString& toml);
QString tomlString(const QString& s);
} // namespace AgentConnect

} // namespace er
