// MCP: the handshake and the tool list (the Mac's 43), calling tools on a real project, errors in the
// Mac's words, and the HTTP transport (local only).
#include "mcp/MCPServer.h"
#include "engine/Transcripts.h"
#include "mcp/MagicAgent.h"
#include "mcp/MCPTools.h"
#include "model/Json.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTest>

using namespace er;

class TestMCP : public QObject {
    Q_OBJECT
    std::unique_ptr<Controller> c;
    std::unique_ptr<MCPServer> server;
    static QString clip(const char* n) { return QStringLiteral(ER_TEST_MEDIA "/") + QLatin1String(n); }

    QJsonObject call(const QString& tool, const QJsonObject& args) {
        return server->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", args}}}}, {}).value("result").toObject();
    }
    QJsonValue result(const QJsonObject& r) {
        return QJsonDocument::fromJson(r.value("content").toArray().first().toObject().value("text").toString().toUtf8()).isObject()
            ? QJsonValue(QJsonDocument::fromJson(r.value("content").toArray().first().toObject().value("text").toString().toUtf8()).object())
            : QJsonValue(QJsonDocument::fromJson(r.value("content").toArray().first().toObject().value("text").toString().toUtf8()).array());
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        if (!QFile::exists(clip("a_main.mp4"))) QSKIP("Run scripts/make-test-clips.py --set tests/media first");
        c = std::make_unique<Controller>();
        server = std::make_unique<MCPServer>(*c);
    }

    void handshake() {
        auto init = MCPHandler::handshake(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-03-26"}}}});
        QVERIFY(init);
        QCOMPARE(init->value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-03-26"));
        QCOMPARE(init->value("result").toObject().value("serverInfo").toObject().value("name").toString(), QStringLiteral("editroom"));
        auto list = MCPHandler::handshake(QJsonObject{{"id", 2}, {"method", "tools/list"}});
        QCOMPARE(list->value("result").toObject().value("tools").toArray().size(), 43);
        QVERIFY(!MCPHandler::handshake(QJsonObject{{"id", 3}, {"method", "tools/call"}}));
        QCOMPARE(MCPServer::displayName("claude-code"), QStringLiteral("Claude"));
        QCOMPARE(MCPServer::displayName("my_agent"), QStringLiteral("My agent"));
    }

    void edits() {
        auto imported = call("import_media", QJsonObject{{"paths", QJsonArray{clip("a_main.mp4"), clip("b_tone.wav")}}});
        QVERIFY2(!imported.value("isError").toBool(), qPrintable(json::write(imported, false)));
        const QJsonArray assets = result(imported).toObject().value("imported").toArray();
        QCOMPARE(assets.size(), 2);
        const QString video = assets[0].toObject().value("id").toString();
        auto added = call("add_clip", QJsonObject{{"asset_id", video}, {"duration", 2.5}});
        QVERIFY(!added.value("isError").toBool());
        const QJsonArray clips = result(added).toObject().value("added").toArray();
        QCOMPARE(clips.size(), 2); // the video and its linked sound
        QCOMPARE(clips[0].toObject().value("track").toString(), QStringLiteral("V1"));
        QCOMPARE(clips[0].toObject().value("end").toDouble(), 2.5);
        const QString id = clips[0].toObject().value("id").toString();
        QVERIFY(!call("split_clip", QJsonObject{{"clip_id", id}, {"at", 1.0}}).value("isError").toBool());
        auto project = result(call("get_project", {})).toObject();
        QCOMPARE(project.value("clips").toArray().size(), 4);
        QCOMPARE(project.value("sequence").toObject().value("duration").toDouble(), 2.5);
        QVERIFY(!call("set_grade", QJsonObject{{"clip_id", id}, {"look", "Mono"}, {"exposure", 0.5}}).value("isError").toBool());
        QCOMPARE(c->model().project().clip(*json::parseUuid(id))->grade->look, std::optional<QString>("Mono"));
        // One undo step per call.
        QVERIFY(!call("history", QJsonObject{{"action", "undo"}}).value("isError").toBool());
        QVERIFY(!c->model().project().clip(*json::parseUuid(id))->grade);
        auto title = call("add_title", QJsonObject{{"text", "Hello"}, {"start", 0.5}});
        QVERIFY(!title.value("isError").toBool());
        QCOMPARE(result(title).toObject().value("added").toObject().value("track").toString(), QStringLiteral("V2"));
        // A frame of the cut.
        auto frame = call("get_frame", QJsonObject{{"time", 0.8}});
        QCOMPARE(frame.value("content").toArray()[0].toObject().value("mimeType").toString(), QStringLiteral("image/png"));
        QVERIFY(frame.value("content").toArray()[0].toObject().value("data").toString().size() > 1000);
        auto sheet = call("look_at_media", QJsonObject{{"asset_id", video}, {"frames", 4}});
        QCOMPARE(sheet.value("content").toArray()[0].toObject().value("mimeType").toString(), QStringLiteral("image/jpeg"));
        // Errors, in the Mac's words.
        auto bad = call("trim_clip", QJsonObject{{"clip_id", "nope"}});
        QVERIFY(bad.value("isError").toBool());
        QCOMPARE(bad.value("content").toArray()[0].toObject().value("text").toString(), QStringLiteral("No clip with id nope. get_project has them."));
        QCOMPARE(call("nonsense", {}).value("content").toArray()[0].toObject().value("text").toString(), QStringLiteral("No tool called nonsense."));
        auto marker = result(call("set_marker", QJsonObject{{"time", 1.0}, {"name", "Drop"}, {"color", "green"}})).toObject();
        QCOMPARE(marker.value("color").toString(), QStringLiteral("green"));
    }

    // A Magic run: the agent's edits go to a draft and land as one undo step when it finishes.
    void magicRun() {
        qputenv("PATH", QByteArray(ER_TEST_DATA "/fake-agents:") + qgetenv("PATH"));
        QVERIFY(server->start());
        MagicAgent agent(*c, *server);
        QSignalSpy done(&agent, &MagicAgent::finished);
        const size_t before = c->model().project().clips.size();
        const size_t undo = c->model().undoDepth();
        agent.start(QStringLiteral("add a title"), MagicAgent::Kind::claude);
        QVERIFY(agent.running());
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
        QCOMPARE(done.first().at(0).toString(), QStringLiteral("I added a title at the start."));
        QVERIFY(done.first().at(1).toBool());
        QCOMPARE(c->model().project().clips.size(), before + 1);
        QCOMPARE(c->model().undoDepth(), undo + 1);
        // The brief: the Mac's words, for this computer.
        const QString brief = MagicAgent::brief(QStringLiteral("make it fast"), c->model().project());
        QVERIFY(brief.startsWith(QStringLiteral("You're the editor in Editroom, the video editor open on this computer.")));
        QVERIFY(brief.contains(QStringLiteral("What they asked for: \"make it fast\"")));
    }

    void codexConfig() {
        const QString toml = QStringLiteral("model = \"x\"\n\n[mcp_servers.editroom]\ncommand = \"a\"\n\n[mcp_servers.editroom.env]\nA = 1\n\n[other]\nb = 2\n\n");
        QCOMPARE(AgentConnect::removingEditroom(toml), QStringLiteral("model = \"x\"\n\n[other]\nb = 2"));
        QCOMPARE(AgentConnect::tomlString(QStringLiteral("a\"b\\c")), QStringLiteral("\"a\\\"b\\\\c\""));
    }

    void http() {
        QVERIFY(server->start());
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, server->listeningPort());
        QVERIFY(s.waitForConnected(2000));
        const QByteArray body = R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"clientInfo":{"name":"claude-code"}}})";
        s.write("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QByteArray resp;
        connect(&s, &QTcpSocket::readyRead, this, [&] { resp += s.readAll(); });
        QTRY_VERIFY_WITH_TIMEOUT(resp.contains("\r\n\r\n{"), 5000);
        QVERIFY2(resp.startsWith("HTTP/1.1 200 OK"), resp.constData());
        QVERIFY(resp.contains("Mcp-Session-Id: "));
        QCOMPARE(server->clients().begin()->second.name, QStringLiteral("Claude"));
        // Web pages can't reach in.
        QTcpSocket w;
        w.connectToHost(QHostAddress::LocalHost, server->listeningPort());
        QVERIFY(w.waitForConnected(2000));
        w.write("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\nContent-Length: 2\r\n\r\n{}");
        QByteArray r2;
        connect(&w, &QTcpSocket::readyRead, this, [&] { r2 += w.readAll(); });
        QTRY_VERIFY_WITH_TIMEOUT(r2.contains("\r\n\r\n"), 5000);
        QVERIFY(r2.startsWith("HTTP/1.1 403"));
    }

    /// An agent captions what's said (whisper.cpp's JFK sample), and reads the transcript.
    void captionsFromSpeech() {
        if (!Transcripts::available()) QSKIP("No speech model: set EDITROOM_WHISPER_MODEL");
        auto imported = call("import_media", QJsonObject{{"paths", QJsonArray{QStringLiteral(ER_SOURCE "/third_party/whisper.cpp/samples/jfk.wav")}}});
        const QString id = result(imported).toObject().value("imported").toArray().first().toObject().value("id").toString();
        auto added = call("add_clip", QJsonObject{{"asset_id", id}});
        QVERIFY(!added.value("isError").toBool());
        const QString clip = result(added).toObject().value("added").toArray().first().toObject().value("id").toString();
        auto t = result(call("get_transcript", QJsonObject{{"asset_id", id}})).toObject();
        QCOMPARE(t.value("status").toString(), QStringLiteral("ready"));
        QVERIFY(!t.value("sentences").toArray().isEmpty());
        auto caps = call("set_captions", QJsonObject{{"clip_ids", QJsonArray{clip}}});
        QVERIFY2(!caps.value("isError").toBool(), qPrintable(json::write(caps, false)));
        const QJsonObject c = result(caps).toObject().value("captions").toObject();
        QVERIFY(c.value("count").toInt() > 2);
        auto found = result(call("search_media", QJsonObject{{"said", "your country"}})).toObject();
        bool saidIt = false;
        for (const auto& h : found.value("found").toArray()) if (!h.toObject().value("said").toArray().isEmpty()) saidIt = true;
        QVERIFY(saidIt);
    }

    void zoomTool() {
        // set_transform's zoom (in at 1s over 1.5s, framing the right side), reported by get_project, an out that ends
        // with the clip, and off keeping the scale; a mode it doesn't know is refused.
        Project p;
        Clip clip(p.videoTracks[0].id, ClipKind::solid, 0, 6);
        p.clips = {clip};
        c->model().load(p, {});
        const QString id = clip.id.toString(QUuid::WithoutBraces).toUpper();
        auto err = [](const QJsonObject& r) { return r.value("isError").toBool(); };
        QVERIFY(!err(call("set_transform", QJsonObject{{"clip_id", id}, {"zoom", "in"}, {"zoom_start", 1.0}, {"zoom_seconds", 1.5}, {"focus_x", 0.75}})));
        auto z = c->model().project().clip(clip.id)->transform;
        QVERIFY(z && z->zoom);
        QCOMPARE(z->scale, 2.0);
        QVERIFY(z->zoom->mode == ZoomMove::Mode::zoomIn);
        QCOMPARE(z->zoom->p1, 1.0);
        QCOMPARE(z->zoom->p2, 2.5);
        QVERIFY(std::abs(z->x + 0.5) < 1e-9);
        QVERIFY(!z->at(0));
        QCOMPARE(z->at(3)->scale, 2.0);
        const QString shown = call("get_project", {}).value("content").toArray().first().toObject().value("text").toString();
        QVERIFY(shown.contains(QStringLiteral("\"zoom\"")));
        QVERIFY(!err(call("set_transform", QJsonObject{{"clip_id", id}, {"zoom", "out"}, {"zoom_seconds", 2.0}, {"zoom_smooth", false}})));
        const auto o = c->model().project().clip(clip.id)->transform->zoom;
        QVERIFY(o && o->mode == ZoomMove::Mode::zoomOut && o->p3 == 4 && o->p4 == 6 && !o->smooth);
        QVERIFY(!err(call("set_transform", QJsonObject{{"clip_id", id}, {"zoom", "off"}})));
        QVERIFY(!c->model().project().clip(clip.id)->transform->zoom);
        QCOMPARE(c->model().project().clip(clip.id)->transform->scale, 2.0);
        QVERIFY(err(call("set_transform", QJsonObject{{"clip_id", id}, {"zoom", "sideways"}})));
    }

    void startupLists() {
        // Asked by agents as they start (Codex): answered, empty, without opening Editroom.
        for (const char* m : {"resources/list", "resources/templates/list", "prompts/list"}) {
            QVERIFY(MCPHandler::isStartup(QLatin1String(m)));
            const auto r = MCPHandler::handshake(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", m}});
            QVERIFY(r && r->contains("result"));
        }
        QVERIFY(!MCPHandler::isStartup(QStringLiteral("tools/call")));
    }
};

QTEST_MAIN(TestMCP)
#include "tst_mcp.moc"
