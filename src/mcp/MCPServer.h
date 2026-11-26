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
QJsonObject error(const QJsonValue& id, int code, const QString& message);
}

class MCPServer : public QObject {
    Q_OBJECT
public:
    static constexpr quint16 port = 47920;
    struct Client { QString id, name; QDateTime lastSeen; };
    explicit MCPServer(Controller& c, QObject* parent = nullptr);
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
    Controller& c_;
    QTcpServer server_;
    QString problem_;
    std::map<QString, Client> clients_;
    bool enabled_ = true;
};

/// `editroom-lx --mcp`: stdin/stdout lines to and from the running app's server; hello, ping and the
/// tool list answered here while Editroom is closed; the app launched on the first real call.
int runMCPBridge(int argc, char** argv);

} // namespace er
