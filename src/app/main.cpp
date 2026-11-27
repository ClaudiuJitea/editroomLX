// editroom-lx: the editor, plus the headless tools the tests, syncs and benchmarks use.
#include "model/Codec.h"
#include "model/Json.h"
#include "model/Shortcuts.h"
#include <QJsonArray>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QSettings>
#include <QMenuBar>
#include "ui/MainWindow.h"
#include "mcp/MCPServer.h"
#include "Bench.h"
#include "SelfTest.h"
#include "ui/Updates.h"
#include "SingleInstance.h"
#include <QIcon>
#include "mcp/MCPTools.h"
#include "mcp/MagicPopover.h"
#include "ui/Theme.h"
#include "ui/SettingsDialog.h"
#include "model/Timecode.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <chrono>
#include <thread>
#include "audio/AudioStream.h"

using namespace er;

static void log(const QString& s) { std::fprintf(stdout, "%s\n", s.toUtf8().constData()); std::fflush(stdout); }

/// `--roundtrip <in.room> [out.room]`: open, save, re-open; checks the result is semantically equal to the input.
static int roundtrip(const QString& in, const QString& out) {
    QFile f(in);
    if (!f.open(QIODevice::ReadOnly)) { log(QStringLiteral("ROUNDTRIP FAIL cannot read %1").arg(in)); return 1; }
    const QByteArray original = f.readAll();
    auto res = codec::decodeDocument(original);
    if (!res.document) { log(QStringLiteral("ROUNDTRIP FAIL %1: %2").arg(in, res.error)); return 1; }
    const QByteArray saved = codec::saveBytes(*res.document);
    auto again = codec::decodeDocument(saved);
    const bool reopened = again.document && *again.document == *res.document;
    const bool same = json::semanticallyEqual(QJsonDocument::fromJson(original).object(), QJsonDocument::fromJson(saved).object());
    const bool bytes = saved == original;
    if (!out.isEmpty()) {
        QFile o(out);
        if (o.open(QIODevice::WriteOnly)) o.write(saved);
    }
    log(QStringLiteral("ROUNDTRIP %1 %2 reopened=%3 semantic=%4 bytes=%5")
            .arg(reopened && same ? "OK" : "FAIL", in, reopened ? "true" : "false", same ? "true" : "false", bytes ? "true" : "false"));
    return reopened && same ? 0 : 1;
}


/// Keys each Codable type writes, from instances with every optional set: `SCHEMA <Type> <key,key…>`.
/// scripts/parity-diff.py compares these with the stored properties of the Swift structs.
static int dumpSchema() {
    Project p;
    MediaAsset a; a.relativePath = ""; a.playablePath = ""; a.bookmark = QByteArray("x");
    Clip c; c.assetID = newUuid(); c.fadeInCurve = FadeCurve::linear; c.fadeOutCurve = FadeCurve::linear; c.linkID = newUuid();
    c.grade = ColorGrade(); c.grade->look = "Mono"; c.transform = ClipTransform(); c.transform->zoom = ZoomMove();
    c.transitionIn = Transition(); c.freeze = true; c.reversedFrom = ReverseSource(); c.reversed = true; c.ramp = SpeedRamp();
    c.captioned = true;
    p.videoTracks[0].duckUnder = newUuid();
    p.clips = {c}; p.assets = {a};
    p.exportName = ""; p.exportDir = ""; p.markers = std::vector<Marker>{Marker()}; p.inPoint = 0; p.outPoint = 1;
    p.magicHistory = std::vector<MagicHistoryEntry>{MagicHistoryEntry()}; p.captions = CaptionTrack();
    CaptionFix fix; fix.merged = true;
    p.captionFixes = std::map<Uuid, std::vector<CaptionFix>>{{newUuid(), {fix}}};
    MediaFolder f; f.relativePath = ""; f.bookmark = QByteArray("x");
    p.folders = std::vector<MediaFolder>{f}; p.hiddenMedia = std::vector<QString>{""};
    const QJsonObject doc = codec::encode(SavedDocument{1, p, {}});
    auto keys = [](const QJsonObject& o) { QStringList k = o.keys(); std::sort(k.begin(), k.end()); return k.join(u','); };
    auto line = [&](const char* type, const QJsonObject& o) { log(QStringLiteral("SCHEMA %1 %2").arg(QLatin1String(type), keys(o))); };
    const QJsonObject po = doc.value("project").toObject();
    const QJsonObject co = po.value("clips").toArray().first().toObject();
    const QJsonObject cap = po.value("captions").toObject();
    line("SavedDocument", doc);
    line("Project", po);
    line("Track", po.value("videoTracks").toArray().first().toObject());
    line("Clip", co);
    line("RGBA", co.value("color").toObject());
    line("TextStyle", co.value("text").toObject());
    line("ColorGrade", co.value("grade").toObject());
    line("WheelPoint", co.value("grade").toObject().value("shadows").toObject());
    line("ClipTransform", co.value("transform").toObject());
    line("ZoomMove", co.value("transform").toObject().value("zoom").toObject());
    line("Transition", co.value("transitionIn").toObject());
    line("ReverseSource", co.value("reversedFrom").toObject());
    line("SpeedRamp", co.value("ramp").toObject());
    line("MediaAsset", po.value("assets").toArray().first().toObject());
    line("Marker", po.value("markers").toArray().first().toObject());
    line("Entry", po.value("magicHistory").toArray().first().toObject());
    line("CaptionTrack", cap);
    line("CaptionStyle", cap.value("style").toObject());
    line("CaptionFix", po.value("captionFixes").toArray().at(1).toArray().first().toObject());
    line("MediaFolder", po.value("folders").toArray().first().toObject());
    Transcript t; t.words = {{0, 1, "x"}};
    const QJsonObject to = codec::encode(t);
    line("Transcript", to);
    line("Word", to.value("words").toArray().first().toObject());
    return 0;
}

/// `SHORTCUT <raw>|<group>|<inMenu>|<title>|<mac combos>`, the same fields scripts/parity-diff.py reads from Shortcuts.swift.
static int dumpShortcuts() {
    for (auto a : Shortcut::allCases()) {
        QStringList combos;
        for (const auto& k : Shortcut::defaults(a)) combos << k.macDisplay();
        log(QStringLiteral("SHORTCUT %1|%2|%3|%4|%5").arg(Shortcut::rawValue(a), Shortcut::group(a),
            Shortcut::inMenu(a) ? "menu" : "-", Shortcut::title(a), combos.join(u' ')));
    }
    return 0;
}

/// `--dump-menu`: the menus as `MENU` lines (title, shortcut, enabled), the same shape as the Mac's, for
/// scripts/parity-diff.py.
static int dumpMenu(er::MainWindow& w) {
    for (QAction* top : w.menuBar()->actions()) {
        log(QStringLiteral("MENU %1").arg(top->text()));
        if (!top->menu()) continue;
        for (QAction* a : top->menu()->actions()) {
            if (a->isSeparator()) { log(QStringLiteral("MENU    -")); continue; }
            QString text = a->text(), key = a->shortcut().toString();
            if (text.contains(u'\t')) { key = text.section(u'\t', 1); text = text.section(u'\t', 0, 0); }
            log(QStringLiteral("MENU    %1 [%2]%3").arg(text, key, a->isEnabled() ? QString() : QStringLiteral(" (off)")));
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    // `--mcp`: a go-between for an AI agent, with no window.
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--mcp") return er::runMCPBridge(argc, argv);
    QStringList raw;
    for (int i = 0; i < argc; ++i) raw << QString::fromLocal8Bit(argv[i]);
    const bool headless = raw.contains(QStringLiteral("--version")) || raw.contains(QStringLiteral("--roundtrip")) ||
                          raw.contains(QStringLiteral("--dump-schema")) || raw.contains(QStringLiteral("--dump-shortcuts")) ||
                          raw.contains(QStringLiteral("--dump-mcp-tools"));
    // `--sound-delay`: the output Editroom plays to, and how long its sound takes to be heard (Bluetooth headphones
    // add 0.15–0.3 s), as the picture allows for it.
    if (raw.contains(QStringLiteral("--sound-delay"))) {
        er::AudioStream stream;
        stream.play(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::printf("SOUND device=\"%s\" buffer=%.0f ms output=%.0f ms total=%.0f ms\n", qPrintable(stream.deviceName()),
                    (stream.outputLatency() - stream.sinkLatency()) * 1000, stream.sinkLatency() * 1000, stream.outputLatency() * 1000);
        stream.pause();
        return 0;
    }
    // `--bench`: drawing titles needs a GUI application, which runs offscreen here.
    if (const auto b = raw.indexOf(QStringLiteral("--bench")); b >= 0) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        QGuiApplication::setApplicationName(QStringLiteral("Editroom"));
        QGuiApplication::setOrganizationName(QStringLiteral("Editroom"));
        er::Theme::loadFonts();
        return er::runBench(b + 1 < raw.size() ? raw[b + 1] : QStringLiteral("bench-media"));
    }
    // `--selftest <mediaDir> <outDir>` and `--format-check <outDir> <file>…`: headless engine checks.
    const auto st = raw.indexOf(QStringLiteral("--selftest")), fc = raw.indexOf(QStringLiteral("--format-check"));
    if ((st >= 0 && st + 2 < raw.size()) || (fc >= 0 && fc + 2 < raw.size())) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        QGuiApplication::setApplicationName(QStringLiteral("Editroom"));
        QGuiApplication::setOrganizationName(QStringLiteral("Editroom"));
        er::Theme::loadFonts();
        er::prepareHardwareVideo();
        if (st >= 0) return er::runSelfTest(raw[st + 1], raw[st + 2]);
        return er::runFormatCheck(raw[fc + 1], raw.mid(fc + 2));
    }
    if (headless) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("Editroom"));
        QCoreApplication::setOrganizationName(QStringLiteral("Editroom"));
        QCoreApplication::setApplicationVersion(QStringLiteral(EDITROOM_VERSION));
        const QStringList args = app.arguments();
        if (args.contains(QStringLiteral("--version"))) {
            log(QStringLiteral("Editroom LX %1 (Mac baseline %2)").arg(EDITROOM_VERSION, EDITROOM_MAC_BASELINE));
            return 0;
        }
        if (args.contains(QStringLiteral("--dump-schema"))) return dumpSchema();
        if (args.contains(QStringLiteral("--dump-shortcuts"))) return dumpShortcuts();
        if (args.contains(QStringLiteral("--dump-mcp-tools"))) {
            for (const auto& t : er::MCPTools::definitions()) log(QStringLiteral("TOOL ") + t.toObject().value("name").toString());
            return 0;
        }
        const auto i = args.indexOf(QStringLiteral("--roundtrip"));
        if (i + 1 < args.size()) return roundtrip(args[i + 1], i + 2 < args.size() ? args[i + 2] : QString());
        return 1;
    }
    // The viewer's Vulkan window is the one native child: its siblings stay ordinary widgets (on Wayland each native
    // widget is a subsurface, and with Qt drawing the title bar they ended up misplaced, the title bar drawn twice).
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Editroom"));
    QApplication::setOrganizationName(QStringLiteral("Editroom"));
    QApplication::setApplicationVersion(QStringLiteral(EDITROOM_VERSION));
    // The app's own name on the desktop (the launcher, its window and the dock): EditroomLX, with its own id, so it's
    // never taken for another Editroom. Settings and projects stay under "Editroom" (the Mac's names).
    QApplication::setApplicationDisplayName(QStringLiteral("EditroomLX"));
    QApplication::setDesktopFileName(QStringLiteral("editroomlx"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/editroom.png")));
    const QStringList args = app.arguments();
    // One Editroom at a time: a later launch hands its files to the running copy (test launches don't).
    const bool testing = args.contains(QStringLiteral("--open")) || args.contains(QStringLiteral("--screenshot")) ||
                         args.contains(QStringLiteral("--dump-menu")) || args.contains(QStringLiteral("--selftest"));
    er::SingleInstance single;
    if (!testing) {
        QStringList handed;
        for (qsizetype k = 1; k < args.size(); ++k) if (!args[k].startsWith(QLatin1String("--"))) handed << args[k];
        if (er::SingleInstance::handOff(handed)) return 0;
        single.listen();
    }
    er::Theme::loadFonts();
    er::Theme::setCurrent(er::Theme::saved());
    er::Theme::apply();
    QApplication::setFont(er::Theme::font(13));
    er::Timecode::setCurrentStyle(QSettings().value(QLatin1String(er::Timecode::styleKey)).toString() == QLatin1String("simple") ? er::TimeStyle::simple : er::TimeStyle::timecode);
    er::Controller controller;
    er::MainWindow window(controller);
    er::MCPServer mcp(controller);
    mcp.setEnabled(QSettings().value(QStringLiteral("agentsEnabled"), true).toBool());
    mcp.start();
    er::MagicAgent agent(controller, mcp);
    er::SettingsDialog::agentApps() = {
        {QStringLiteral("Claude Code"), [] { return !er::MagicAgent::cli(er::MagicAgent::Kind::claude).isEmpty(); },
         [] { return er::AgentConnect::claudeCodeConnected(); }, er::AgentConnect::connectClaudeCode, er::AgentConnect::disconnectClaudeCode,
         QStringLiteral("Adds Editroom to Claude Code for all your projects."), QStringLiteral("Connected. New Claude Code sessions can edit here.")},
        {QStringLiteral("ChatGPT (Codex)"), [] { return !er::MagicAgent::cli(er::MagicAgent::Kind::chatgpt).isEmpty(); },
         [] { return er::AgentConnect::codexConnected(); }, er::AgentConnect::connectCodex, er::AgentConnect::disconnectCodex,
         QStringLiteral("Adds Editroom to Codex."), QStringLiteral("Connected. ChatGPT can edit here, and Magic can use it.")},
    };
    er::SettingsDialog::server() = {
        [&mcp] { return mcp.enabled(); },
        [&mcp](bool on) { mcp.setEnabled(on); QSettings().setValue(QStringLiteral("agentsEnabled"), on); },
        [&mcp] { return mcp.problem(); },
        [&mcp] {
            std::vector<er::MCPServer::Client> list;
            for (const auto& [id, c] : mcp.clients()) if (c.lastSeen.secsTo(QDateTime::currentDateTime()) < 60) list.push_back(c);
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.lastSeen > b.lastSeen; });
            QStringList names;
            for (const auto& c : list) names << c.name;
            return names;
        },
        [&mcp] { return mcp.endpoint(); },
    };
    QObject::connect(&window, &er::MainWindow::magicRequested, &window, [&] {
        auto* pop = new er::MagicPopover(controller, agent, &window);
        pop->setAttribute(Qt::WA_DeleteOnClose);
        pop->show();
    });
    if (args.contains(QStringLiteral("--dump-menu"))) return dumpMenu(window);
    const auto open = args.indexOf(QStringLiteral("--open"));
    controller.model().testLaunch = open >= 0 || args.contains(QStringLiteral("--selftest"));
    QStringList files;
    for (qsizetype k = 1; k < args.size(); ++k) if (!args[k].startsWith(QLatin1String("--")) && (k == 0 || args[k - 1] != QLatin1String("--open"))) files << args[k];
    if (open >= 0 && open + 1 < args.size()) {
        controller.model().open(args[open + 1]);
    } else if (!files.isEmpty()) {
        for (const QString& f : files) {
            if (er::EditorModel::isProjectFile(f)) controller.model().open(f);
            else controller.importFiles({f});
        }
    } else {
        controller.model().restoreLastSession();
    }
    controller.model().startAutosaveTimer();
    QObject::connect(&single, &er::SingleInstance::opened, &window, [&controller, &window](const QStringList& opened) {
        for (const QString& f : opened) {
            if (er::EditorModel::isProjectFile(f)) { if (controller.model().confirmDiscard()) controller.model().open(f); }
            else controller.importFiles({f});
        }
        if (window.isMinimized()) window.showNormal();
        window.raise();
        window.activateWindow();
    });
    // The first launch fills the screen (Wayland doesn't let clients place windows; maximised is closest).
    QSettings s;
    if (!s.value(QStringLiteral("launchedBefore")).toBool()) {
        s.setValue(QStringLiteral("launchedBefore"), true);
        window.showMaximized();
    } else {
        window.show();
    }
    // Automatic update checks, if turned on in Settings › Updates (once a day at most), a little after starting.
    if (!testing) QTimer::singleShot(5000, &window, [&window] { er::Updates::shared().checkIfDue(&window); });
    // `--screenshot <file.png> [--seek t]` (testing): the window as drawn, after things have loaded.
    if (const auto sh = args.indexOf(QStringLiteral("--screenshot")); sh >= 0 && sh + 1 < args.size()) {
        const QString out = args[sh + 1];
        const auto sk = args.indexOf(QStringLiteral("--seek"));
        const double seekTo = sk >= 0 && sk + 1 < args.size() ? args[sk + 1].toDouble() : -1;
        if (!window.isVisible()) window.show();
        window.resize(1480, 940);
        QTimer::singleShot(1500, &window, [&controller, seekTo] { if (seekTo >= 0) controller.model().seek(seekTo); });
        QTimer::singleShot(4500, &window, [&window, out] { window.grab().save(out); QApplication::quit(); });
    }
    return app.exec();
}
