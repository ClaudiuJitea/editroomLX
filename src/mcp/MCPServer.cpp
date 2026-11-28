#include "MCPServer.h"
#include <QJsonArray>
#include "MCPTools.h"
#include "model/Json.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <cstdio>
#include <iostream>

namespace er {

static const QStringList kVersions = {"2025-06-18", "2025-03-26", "2024-11-05"};

QJsonObject MCPHandler::error(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id.isUndefined() ? QJsonValue() : id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

std::optional<QJsonObject> MCPHandler::handshake(const QJsonObject& req) {
    const QJsonValue id = req.value("id");
    const QJsonObject params = req.value("params").toObject();
    auto ok = [&](const QJsonObject& result) { return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; };
    const QString method = req.value("method").toString();
    if (method == QLatin1String("initialize")) {
        const QString asked = params.value("protocolVersion").toString(kVersions[0]);
        return ok(QJsonObject{{"protocolVersion", kVersions.contains(asked) ? asked : kVersions[0]},
                              {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                              {"serverInfo", QJsonObject{{"name", "editroom"}, {"title", "EditroomLX"}, {"version", "1.0"}}},
                              {"instructions", MCPTools::instructions()}});
    }
    if (method == QLatin1String("ping")) return ok(QJsonObject{});
    if (method == QLatin1String("tools/list")) return ok(QJsonObject{{"tools", MCPTools::definitions()}});
    // Asked by some agents as they start (Codex): Editroom has neither, and saying so mustn't open it.
    if (method == QLatin1String("resources/list")) return ok(QJsonObject{{"resources", QJsonArray{}}});
    if (method == QLatin1String("resources/templates/list")) return ok(QJsonObject{{"resourceTemplates", QJsonArray{}}});
    if (method == QLatin1String("prompts/list")) return ok(QJsonObject{{"prompts", QJsonArray{}}});
    return std::nullopt;
}

bool MCPHandler::opensEditroom(const QString& method) { return method == QLatin1String("tools/call"); }

MCPServer::MCPServer(Controller& c, QObject* parent) : QObject(parent), c_(&c) {
    connect(&server_, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = server_.nextPendingConnection()) serve(s);
    });
}

bool MCPServer::start() {
    if (server_.isListening()) return true;
    // This computer only: never reachable from the network.
    if (server_.listen(QHostAddress::LocalHost, port)) { problem_.clear(); return true; }
    if (server_.listen(QHostAddress::LocalHost, 0)) {
        problem_ = QStringLiteral("Another copy of EditroomLX is answering AI apps; this one only runs its own Magic.");
        return true;
    }
    problem_ = QStringLiteral("Couldn't listen on port %1: %2").arg(port).arg(server_.errorString());
    return false;
}

void MCPServer::stop() { server_.close(); }

QString MCPServer::displayName(const QString& raw) {
    const QString s = raw.toLower();
    if (s.contains(QLatin1String("claude"))) return QStringLiteral("Claude");
    if (s.contains(QLatin1String("codex"))) return QStringLiteral("Codex");
    if (s.contains(QLatin1String("chatgpt")) || s.contains(QLatin1String("openai"))) return QStringLiteral("ChatGPT");
    QString w = raw;
    w.replace(u'-', u' ').replace(u'_', u' ');
    return w.left(1).toUpper() + w.mid(1);
}

QJsonObject MCPServer::handle(const QJsonObject& req, const QString& session, const QString& draftToken) {
    if (auto r = MCPHandler::handshake(req)) return *r;
    const QJsonValue id = req.value("id");
    const QJsonObject params = req.value("params").toObject();
    if (req.value("method").toString() != QLatin1String("tools/call")) return MCPHandler::error(id, -32601, QStringLiteral("Method not found"));
    const QString name = params.value("name").toString();
    const QString who = clients_.count(session) ? clients_[session].name : QStringLiteral("Agent");
    MCPDraft* draft = nullptr;
    if (!draftToken.isEmpty()) {
        draft = MCPDraft::find(draftToken);
        if (!draft) return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", MCPTools::failure(QStringLiteral("This Magic edit was stopped or has finished. Stop now."))}};
    }
    emit toolCalled(name, draftToken);
    QJsonObject result;
    try {
        result = QJsonObject{{"content", MCPTools::call(name, params.value("arguments").toObject(), *c_, who, draft)}, {"isError", false}};
    } catch (const MCPToolError& e) {
        result = MCPTools::failure(e.message);
    }
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

void MCPServer::serve(QTcpSocket* s) {
    auto buf = std::make_shared<QByteArray>();
    connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
        buf->append(s->readAll());
        const qsizetype end = buf->indexOf("\r\n\r\n");
        if (end < 0) { if (buf->size() > (32 << 20)) s->close(); return; }
        const QList<QByteArray> lines = buf->left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        if (first.size() < 2) { s->close(); return; }
        std::map<QByteArray, QByteArray> h;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype c = lines[i].indexOf(':');
            if (c > 0) h[lines[i].left(c).trimmed().toLower()] = lines[i].mid(c + 1).trimmed();
        }
        const qsizetype length = h.count("content-length") ? h["content-length"].toLongLong() : 0;
        if (buf->size() - (end + 4) < length) return; // the body is still coming
        const QByteArray method = first[0], path = first[1], body = buf->mid(end + 4, length);
        auto send = [s](const QByteArray& status, const QByteArray& b = {}, const std::map<QByteArray, QByteArray>& headers = {}) {
            QByteArray head = "HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(b.size()) + "\r\nConnection: close\r\n";
            for (const auto& [k, v] : headers) head += k + ": " + v + "\r\n";
            s->write(head + "\r\n" + b);
            s->flush();
            s->disconnectFromHost();
        };
        // Only local callers by name, and no web pages: a page in a browser can't reach in.
        const QByteArray hostName = h["host"].split(':').value(0);
        if (hostName != "127.0.0.1" && hostName != "localhost" && hostName != "[::1]") return send("403 Forbidden");
        if (h.count("origin") && !h["origin"].isEmpty()) {
            const QString host = QUrl(QString::fromLatin1(h["origin"])).host();
            if (host != QLatin1String("127.0.0.1") && host != QLatin1String("localhost")) return send("403 Forbidden");
        }
        if (!path.startsWith("/mcp")) return send("404 Not Found");
        // A Magic run's agent works on a hidden draft, named in its address.
        const QString draftToken = QUrlQuery(QUrl(QString::fromLatin1(path)).query()).queryItemValue(QStringLiteral("draft"));
        // With AI Agents off, the connection is only open for this copy's own Magic runs.
        if (!enabled_ && !(MCPDraft::find(draftToken))) return send("403 Forbidden");
        if (method == "DELETE") { clients_.erase(QString::fromLatin1(h["mcp-session-id"])); emit clientsChanged(); return send("200 OK"); }
        if (method != "POST") return send("405 Method Not Allowed", {}, {{"Allow", "POST, DELETE"}});
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            return send("400 Bad Request", json::write(MCPHandler::error(QJsonValue(), -32700, QStringLiteral("Parse error")), false), {{"Content-Type", "application/json"}});
        const QJsonObject req = doc.object();
        QString session = QString::fromLatin1(h["mcp-session-id"]);
        if (req.value("method").toString() == QLatin1String("initialize")) {
            session = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
            const QString client = req.value("params").toObject().value("clientInfo").toObject().value("name").toString(QStringLiteral("Agent"));
            // Magic's own agent isn't listed as a connected app.
            if (draftToken.isEmpty()) { clients_[session] = Client{session, displayName(client), QDateTime::currentDateTime()}; emit clientsChanged(); }
        } else if (!session.isEmpty() && draftToken.isEmpty()) {
            if (!clients_.count(session)) clients_[session] = Client{session, QStringLiteral("Agent"), {}};
            clients_[session].lastSeen = QDateTime::currentDateTime();
        }
        // Notifications get no reply.
        if (!req.contains("id")) return send("202 Accepted");
        const QJsonObject reply = handle(req, session, draftToken);
        std::map<QByteArray, QByteArray> headers{{"Content-Type", "application/json"}};
        if (!session.isEmpty()) headers["Mcp-Session-Id"] = session.toLatin1();
        send("200 OK", json::write(reply, false), headers);
    });
}

// MARK: Bridge

namespace {
/// One POST to the app's server; status 0 when it isn't there.
std::pair<int, QByteArray> post(const QByteArray& body, QString& session, const QByteArray& from) {
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, MCPServer::port);
    if (!s.waitForConnected(1000)) return {0, {}};
    QByteArray req = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(MCPServer::port) +
                     "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: " +
                     QByteArray::number(body.size()) + "\r\n";
    if (!session.isEmpty()) req += "Mcp-Session-Id: " + session.toLatin1() + "\r\n";
    if (!from.isEmpty()) req += "X-Editroom-From: " + from + "\r\n";
    s.write(req + "\r\n" + body);
    QByteArray resp;
    while (s.waitForReadyRead(600000)) resp += s.readAll();
    resp += s.readAll();
    const qsizetype end = resp.indexOf("\r\n\r\n");
    if (end < 0) return {0, {}};
    const QList<QByteArray> lines = resp.left(end).split('\n');
    const int status = lines.value(0).split(' ').value(1).toInt();
    for (const auto& l : lines) if (l.toLower().startsWith("mcp-session-id:")) session = QString::fromLatin1(l.mid(15).trimmed());
    return {status, resp.mid(end + 4)};
}
void writeLine(const QByteArray& reply) {
    QByteArray out = reply;
    out.replace('\n', ' ');
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
} // namespace

int runMCPBridge(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QByteArray from = args.indexOf(QStringLiteral("--from")) >= 0 ? args.value(args.indexOf(QStringLiteral("--from")) + 1).toUtf8() : QByteArray();
    QString session;
    bool launched = false;
    QByteArray heldHello;
    auto launch = [&] {
        if (launched) return;
        launched = true;
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray data = QByteArray::fromStdString(line).trimmed();
        if (data.isEmpty()) continue;
        const QJsonObject json = QJsonDocument::fromJson(data).object();
        const QString method = json.value("method").toString();
        const bool hasId = json.contains("id");
        // Anything but using a tool: Editroom's own answer if it's open; with it closed, answered here (what it would
        // say, or that it doesn't do that), and Editroom isn't opened.
        if (!MCPHandler::opensEditroom(method)) {
            auto [status, body] = post(data, session, from);
            QByteArray reply;
            if (status == 200) reply = body;
            else if (status == 0) {
                if (method == QLatin1String("initialize")) heldHello = data;
                // A reply to something asked of the agent (no method) or a notification needs no answer.
                if (!method.isEmpty() && hasId) {
                    const QJsonObject answer = MCPHandler::handshake(json).value_or(
                        MCPHandler::error(json.value("id"), -32601, QStringLiteral("Method not found: %1").arg(method)));
                    reply = json::write(answer, false);
                }
            } else if (status != 202) reply = body;
            if (!reply.isEmpty() && hasId) writeLine(reply);
            continue;
        }
        QByteArray reply;
        bool answered = false;
        // Up to about 20 seconds for the app to start and begin listening.
        for (int attempt = 0; attempt < 80; ++attempt) {
            if (!heldHello.isEmpty()) {
                if (post(heldHello, session, from).first == 0) { launch(); QThread::msleep(250); continue; }
                heldHello.clear();
            }
            auto [status, body] = post(data, session, from);
            if (status == 200 || status == 202) { reply = status == 200 ? body : QByteArray(); answered = true; launched = false; break; }
            if (status == 0) { launch(); QThread::msleep(250); continue; }
            reply = body;
            answered = true;
            break;
        }
        if (!answered && hasId)
            reply = json::write(MCPHandler::error(json.value("id"), -32000,
                QStringLiteral("EditroomLX isn't answering. Open EditroomLX and turn on AI Agents (Edit menu > Settings… > Magic).")), false);
        if (!reply.isEmpty() && hasId) writeLine(reply);
    }
    return 0;
}

} // namespace er
