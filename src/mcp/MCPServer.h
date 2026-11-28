// MCPServer.swift: MCP's Streamable HTTP transport with plain JSON replies, on this computer only
// (127.0.0.1:47920, or any free port when another copy has it). Local callers by name only and no web
// pages. `MCPHandler::handshake` also answers for the `--mcp` bridge while Editroom is closed.
#pragma once
#include "ui/Controller.h"
#include <QDateTime>
#include <QJsonObject>
#include <QTcpServer>

namespace er {

namespace MCPHandler {
/// initialize, ping and tools/list: needing nothing from the editor.
std::optional<QJsonObject> handshake(const QJsonObject& req);
/// Whether a request needs Editroom itself, and opens it when it's closed: only using a tool does. Everything
/// else an agent sends (its hello, pings, asking what there is, as it starts up or now and then) is answered
/// without it, so an AI app starting or idling never opens Editroom.
bool opensEditroom(const QString& method);
QJsonObject error(const QJsonValue& id, int code, const QString& message);
}

class MCPServer : public QObject {
    Q_OBJECT
public:
    static constexpr quint16 port = 47920;
    struct Client { QString id, name; QDateTime lastSeen; };
    explicit MCPServer(Controller& c, QObject* parent = nullptr);
    /// Only the tab in front talks to the AI apps.
    void setController(Controller& c) { c_ = &c; }
    bool start();
    void stop();
    bool running() const { return server_.isListening(); }
    quint16 listeningPort() const { return server_.serverPort(); }
    QString problem() const { return problem_; }
    const std::map<QString, Client>& clients() const { return clients_; }
    static QString displayName(const QString& raw);
    /// One JSON-RPC request (for tests and the HTTP path).
    QJsonObject handle(const QJsonObject& req, const QString& session, const QString& draftToken = {});
    /// "AI Agents" (other apps may connect); off leaves only this copy's own Magic runs.
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    QString endpoint() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(listeningPort()); }

signals:
    void clientsChanged();
    void toolCalled(const QString& tool, const QString& draftToken);

private:
    void serve(QTcpSocket* s);
    Controller* c_;
    QTcpServer server_;
    QString problem_;
    std::map<QString, Client> clients_;
    bool enabled_ = true;
};

/// `editroom-lx --mcp`: stdin/stdout lines to and from the running app's server. While Editroom is closed,
/// everything but using a tool is answered here, so an agent starting up or idling doesn't open Editroom; the
/// first tool it uses opens it.
int runMCPBridge(int argc, char** argv);

} // namespace er
