#include "MagicAgent.h"
#include "model/MediaFolders.h"
#include <QRegularExpression>
#include <QPointer>
#include <QTimer>
#include "MagicPlan.h"
#include "MagicShow.h"
#include "MagicThinking.h"
#include "model/Json.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

namespace er {

QString MagicAgent::cli(Kind k) {
    const QString exe = k == Kind::claude ? QStringLiteral("claude") : QStringLiteral("codex");
    QStringList dirs = qEnvironmentVariable("PATH").split(u':', Qt::SkipEmptyParts);
    dirs << QDir::homePath() + QStringLiteral("/.local/bin") << QDir::homePath() + QStringLiteral("/.npm-global/bin")
         << QStringLiteral("/usr/local/bin") << QStringLiteral("/usr/bin");
    for (const QString& d : dirs) {
        const QFileInfo f(d + u'/' + exe);
        if (f.isExecutable()) return f.absoluteFilePath();
    }
    return {};
}

std::vector<MagicAgent::ModelOption> MagicAgent::models(Kind k) const {
    if (k == Kind::claude) {
        const QStringList efforts{"low", "medium", "high", "xhigh", "max"};
        return {{"claude-opus-5-5", "Opus 5.5", "Best edits", efforts, {}}, {"claude-sonnet-5-5", "Sonnet 5.5", "Fast", efforts, {}},
                {"claude-haiku-5-5", "Haiku 5.5", "Fastest", efforts, {}}, {"claude-fable-5-2", "Fable 5.2", "Most capable", efforts, {}}};
    }
    // Just these, in this order. Names and efforts come from Codex's own list when it has them.
    const std::vector<std::pair<QString, QString>> picks = {{"gpt-6.2-sol", "GPT-6.2-Sol"}, {"gpt-6.2-astra", "GPT-6.2-Astra"},
                                                            {"gpt-6.1-terra", "GPT-6.1-Terra"}, {"gpt-6-luna", "GPT-6-Luna"}};
    QFile f(QDir::homePath() + QStringLiteral("/.codex/models_cache.json"));
    std::map<QString, QJsonObject> known;
    if (f.open(QIODevice::ReadOnly))
        for (const auto& m : QJsonDocument::fromJson(f.readAll()).object().value("models").toArray())
            known[m.toObject().value("slug").toString()] = m.toObject();
    std::vector<ModelOption> out;
    for (const auto& [slug, name] : picks) {
        auto it = known.find(slug);
        if (!known.empty() && it == known.end()) continue; // this plan doesn't offer it
        QStringList efforts;
        if (it != known.end())
            for (const auto& l : it->second.value("supported_reasoning_levels").toArray()) {
                const QString e = l.toObject().value("effort").toString();
                if (e != QLatin1String("ultra")) efforts << e; // more agents, much more cost
            }
        if (efforts.isEmpty()) efforts = QStringList{"low", "medium", "high", "xhigh"};
        out.push_back({slug, name, {}, efforts, it != known.end() ? it->second.value("default_reasoning_level").toString("medium") : QStringLiteral("medium")});
    }
    return out;
}

QString MagicAgent::model(Kind k) const {
    const auto options = models(k);
    const QString saved = QSettings().value(QStringLiteral("magicModel.") + rawValue(k)).toString();
    for (const auto& o : options) if (o.id == saved) return saved;
    return options.empty() ? QString() : options.front().id;
}

QString MagicAgent::effort(Kind k) const {
    const QString m = model(k);
    const QString saved = QSettings().value(QStringLiteral("magicEffort.") + rawValue(k)).toString();
    for (const auto& o : models(k)) if (o.id == m) return o.efforts.contains(saved) ? saved : o.defaultEffort;
    return {};
}

void MagicAgent::setModel(Kind k, const QString& id, const QString& effort) {
    QSettings s;
    s.setValue(QStringLiteral("magicModel.") + rawValue(k), id);
    s.setValue(QStringLiteral("magicEffort.") + rawValue(k), effort);
}

/// The Mac's words, with the app's Linux name: "Editroom" becomes "EditroomLX" (ids such as "editroom" stay).
static QByteArray branded(QByteArray json) {
    static const QRegularExpression word(QStringLiteral("(?<![\\w/-])Editroom(?![\\w/]| LX)"));
    return QString::fromUtf8(json).replace(word, QStringLiteral("EditroomLX")).replace(QStringLiteral("this Mac"), QStringLiteral("this computer")).toUtf8();
}

static QJsonObject briefText() {
    for (const QString& p : {QCoreApplication::applicationDirPath() + QStringLiteral("/../share/editroom/magic-brief.json"),
                             QStringLiteral(ER_RESOURCES "/magic-brief.json")}) {
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) return QJsonDocument::fromJson(branded(f.readAll())).object();
    }
    return {};
}

QString MagicAgent::brief(const QString& request, const Project& p, const std::optional<MagicPointers>& pointers) {
    static const QJsonObject t = briefText();
    const QString ask = request.trimmed();
    int onTimeline = 0;
    for (const auto& c : p.clips) if (c.kind != ClipKind::audio) ++onTimeline;
    const bool hasEdit = onTimeline > 0;
    QStringList lines{t.value("intro").toString()};
    if (ask.isEmpty()) lines << t.value(hasEdit ? "noAskEdit" : "noAskEmpty").toString();
    else lines << t.value("asked").toString().arg(ask);
    if (pointers) if (auto said = pointing(*pointers, ask, p)) lines << *said;
    if (hasEdit) {
        QString e = t.value("existing").toString();
        e.replace(QStringLiteral("%d"), QString::number(onTimeline)).replace(QStringLiteral("%.0f"), QString::number(std::round(p.duration())));
        lines << e;
    } else {
        lines << t.value("empty").toString();
    }
    lines << t.value("howto").toString();
    return lines.join(QStringLiteral("\n\n"));
}

std::optional<QString> MagicAgent::pointing(const MagicPointers& ptr, const QString& request, const Project& p) {
    const auto labels = MagicMention::clipLabels(p);
    const auto marks = MagicMention::markerLabels(p);
    const auto folders = MagicMention::folderLabels(p);
    const auto mentions = MagicMention::find(request, p.assets, labels, marks, folders);
    if (p.clips.empty() && mentions.empty()) return std::nullopt;
    auto t = [](double s) { return QString::number(s, 'f', 2) + u's'; };
    auto track = [&](const Clip& c) {
        for (size_t i = 0; i < p.videoTracks.size(); ++i) if (p.videoTracks[i].id == c.trackID) return QStringLiteral("V%1").arg(i + 1);
        int a = 0;
        for (size_t i = 0; i < p.audioTracks.size(); ++i) if (p.audioTracks[i].id == c.trackID) { a = int(i); break; }
        return QStringLiteral("A%1").arg(a + 1);
    };
    auto clip = [&](const Clip& c) {
        return QStringLiteral("\"%1\" on %2 %3-%4 (id %5)").arg(p.name(c), track(c), t(c.start), t(c.end()), c.id.toString(QUuid::WithoutBraces).toUpper());
    };
    auto byTrack = [&](std::vector<Clip> v) {
        std::stable_sort(v.begin(), v.end(), [&](const Clip& a, const Clip& b) { return trackOrder(p, a.trackID) < trackOrder(p, b.trackID); });
        QStringList s;
        for (const auto& c : v) s << clip(c);
        return s.join(QStringLiteral("; "));
    };
    QStringList where;
    if (!p.clips.empty()) {
        std::vector<Clip> under;
        for (const auto& c : p.clips) if (c.start <= ptr.playhead + 1e-6 && ptr.playhead < c.end() - 1e-6 && c.kind != ClipKind::audio) under.push_back(c);
        where << QStringLiteral("- Playhead: ") + t(ptr.playhead) + (under.empty() ? QString() : QStringLiteral(", over ") + byTrack(under));
        QStringList sel;
        for (const auto& id : ptr.selected) if (const Clip* c = p.clip(id)) sel << clip(*c);
        where << QStringLiteral("- Selected: ") + (sel.isEmpty() ? QStringLiteral("nothing")
                                                                 : QStringLiteral("%1 clip%2: ").arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s")) + sel.join(QStringLiteral("; ")));
        where << QStringLiteral("- In/out range: ") + (ptr.range ? t(ptr.range->start) + QStringLiteral(" to ") + t(ptr.range->end) : QStringLiteral("none"));
    }
    QStringList meant;
    std::vector<MagicMention> seen;
    for (const auto& f : mentions) {
        if (std::find(seen.begin(), seen.end(), f.mention) != seen.end()) continue;
        seen.push_back(f.mention);
        switch (f.mention.kind) {
        case MagicMention::Kind::playhead: meant << QStringLiteral("@playhead means the playhead above."); break;
        case MagicMention::Kind::selected:
            meant << (ptr.selected.empty() ? QStringLiteral("@selected: nothing was selected, so do the best you can from the rest of what they said.")
                                           : QStringLiteral("@selected means exactly the selected clips above, no others."));
            break;
        case MagicMention::Kind::range:
            meant << (!ptr.range ? QStringLiteral("@in-out: no in/out range was set, so do the best you can from the rest of what they said.")
                                 : QStringLiteral("@in-out means the in/out range above."));
            break;
        case MagicMention::Kind::media:
            if (const MediaAsset* a = p.asset(f.mention.id))
                meant << QStringLiteral("@%1 means the library file %1 (asset_id %2).").arg(a->name, a->id.toString(QUuid::WithoutBraces).toUpper());
            break;
        case MagicMention::Kind::clip:
            if (const Clip* c = p.clip(f.mention.id))
                for (const auto& l : labels) if (l.id == f.mention.id) meant << QStringLiteral("@clip:%1 means exactly one clip on the timeline: %2.").arg(l.name, clip(*c));
            break;
        case MagicMention::Kind::marker:
            for (const auto& l : marks)
                if (l.id == f.mention.id) meant << QStringLiteral("@%1 (or @marker:%1) means the marker \"%1\" at %2 on the timeline.").arg(l.name, t(l.time));
            break;
        case MagicMention::Kind::folder: {
            const MagicMention::FolderLabel* fl = nullptr;
            for (const auto& x : folders) if (x.path == f.mention.path) fl = &x;
            if (!fl) break;
            auto named = [&](const std::vector<Uuid>& ids) {
                std::vector<const MediaAsset*> out;
                for (const Uuid& id : ids) if (const MediaAsset* a = p.asset(id)) out.push_back(a);
                return out;
            };
            auto asText = [](const MediaAsset* a) { return QStringLiteral("%1 (asset_id %2)").arg(a->name, a->id.toString(QUuid::WithoutBraces).toUpper()); };
            const bool only = f.mention.only;
            if ((only || MagicMention::onlyDirect(request)) && !fl->direct.empty()) {
                // They asked for just what's directly in it: said outright, so "all" can't be read as the sub-folders too.
                const auto files = named(fl->direct);
                const int skipped = int(fl->files.size() - files.size());
                QStringList shown;
                for (size_t i = 0; i < files.size() && i < 60; ++i) shown << asText(files[i]);
                const int n = int(files.size());
                meant << QStringLiteral("@folder:%1%2 means ONLY the %3 library file%4 directly in the Media folder \"%5\": ")
                             .arg(fl->name, only ? QStringLiteral(":only") : QString()).arg(n).arg(n == 1 ? QString() : QStringLiteral("s"), fl->label)
                         + shown.join(QStringLiteral(", "))
                         + (n > 60 ? QStringLiteral(", and %1 more (list_media gives each file's folder)").arg(n - 60) : QString())
                         + QStringLiteral(". %1 the folders inside it, so don't use any of the %2 file%3 in its sub-folders. ")
                               .arg(only ? QStringLiteral("That leaves out") : QStringLiteral("They asked to leave out")).arg(skipped).arg(skipped == 1 ? QString() : QStringLiteral("s"))
                         + QStringLiteral("\"All\" in their request means all of these files: every one goes in the edit, even if only briefly.");
            } else {
                // Its files by where they are: directly in it, then each folder inside it, so "only this folder" or "skip
                // the B-roll folder" can be done.
                const MediaTree tree(p);
                const auto files = named(fl->files);
                std::vector<std::pair<QString, std::vector<const MediaAsset*>>> groups;
                for (const MediaAsset* a : files) {
                    const QString at = tree.folderOf(*a).value_or(fl->path);
                    const QString name = at == fl->path ? QStringLiteral("Directly in \"%1\"").arg(fl->label) : QStringLiteral("In \"%1\"").arg(tree.label(at));
                    auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == name; });
                    if (g == groups.end()) groups.push_back({name, {a}}); else g->second.push_back(a);
                }
                int left = 60;
                QStringList listed;
                for (const auto& [name, list] : groups) {
                    QStringList shown;
                    for (size_t i = 0; i < list.size() && int(i) < std::max(0, left); ++i) shown << asText(list[i]);
                    left -= int(shown.size());
                    const int rest = int(list.size()) - int(shown.size());
                    const QString more = rest > 0 ? (shown.isEmpty() ? QString() : QStringLiteral(", ")) + QStringLiteral("and %1 more").arg(rest) : QString();
                    listed << name + QStringLiteral(": ") + shown.join(QStringLiteral(", ")) + more + u'.';
                }
                const int n = int(files.size());
                const QString note = n > 60 ? QStringLiteral(" (list_media gives each file's folder)") : QString();
                meant << QStringLiteral("@folder:%1 means the %2 library file%3 in the Media folder \"%4\" and the folders inside it%5. %6 Use all of them unless they ask otherwise: for example only the files directly in the folder (not the folders inside it), or leaving a folder out. If they ask for all of them, every one of those files goes in the edit, even if only briefly.")
                             .arg(fl->name).arg(n).arg(n == 1 ? QString() : QStringLiteral("s"), fl->label, note, listed.join(u' '));
            }
            break;
        }
        }
    }
    QString out;
    if (!where.isEmpty()) out = QStringLiteral("Where they were in the editor when they asked:\n") + where.join(u'\n') + u'\n';
    if (!meant.isEmpty()) {
        meant.sort();
        out += QStringLiteral("They pointed with @: ") + meant.join(u' ') + QStringLiteral(" Act on exactly what they pointed at.\n");
    }
    QStringList timed;
    const QString req = MagicMention::markersAsTimes(request, p);
    const auto span = MagicTimes::span(req, p.fps);
    if (span) {
        std::vector<Clip> over;
        for (const auto& c : p.clips) if (c.kind != ClipKind::audio && c.start < span->end - 1e-6 && c.end() > span->start + 1e-6) over.push_back(c);
        std::stable_sort(over.begin(), over.end(), [&](const Clip& a, const Clip& b) {
            return std::make_pair(a.start, trackOrder(p, a.trackID)) < std::make_pair(b.start, trackOrder(p, b.trackID));
        });
        QStringList o;
        for (const auto& c : over) o << clip(c);
        timed << QStringLiteral("\"%1\" is %2 to %3 on the timeline").arg(span->text, t(span->start), t(span->end)) + (o.isEmpty() ? QString() : QStringLiteral(", over ") + o.join(QStringLiteral("; "))) + u'.';
    }
    for (const auto& typed : MagicTimes::times(req, p.fps)) {
        if (typed.unitOnly || (span && span->text.contains(typed.text))) continue;
        std::vector<Clip> under;
        for (const auto& c : p.clips) if (c.kind != ClipKind::audio && c.start <= typed.seconds + 1e-6 && typed.seconds < c.end() - 1e-6) under.push_back(c);
        timed << QStringLiteral("\"%1\" is %2 (if it's a place on the timeline").arg(typed.text, t(typed.seconds))
                     + (under.empty() ? QString() : QStringLiteral(", over ") + byTrack(under)) + QStringLiteral("; it may be a length instead).");
    }
    if (!timed.isEmpty()) out += QStringLiteral("Times in their request: ") + timed.join(u' ') + u'\n';
    if (!where.isEmpty())
        out += QStringLiteral("Use the playhead, selection and range only when their words point at them (\"here\", \"this clip\", \"these\", \"this spot\", "
                              "\"from here on\", or an @). Otherwise the request is about the whole edit, whatever happens to be selected.");
    return out.trimmed();
}

MagicAgent::MagicAgent(Controller& c, MCPServer& server, QObject* parent) : QObject(parent), c_(&c), server_(server) {
    connect(&server_, &MCPServer::toolCalled, this, [this](const QString& tool, const QString& token) {
        if (draft_ && token == draft_->token) {
            status_ = name(kind_) + QStringLiteral(" is ") + doing(tool);
            emit statusChanged(status_);
            // The thinking scene follows along: studying the media, then the edit taking shape in the draft.
            if (auto* t = MagicThinking::current()) {
                const QString d = doing(tool);
                if (d == QLatin1String("cutting") || d == QLatin1String("grading") || d == QLatin1String("titling") || !draft_->edits.empty())
                    t->plan(draft_->project);
                else if (tool != QLatin1String("list_media") && tool != QLatin1String("get_project"))
                    t->analyze(std::nullopt);
            }
        }
    });
}

QString MagicAgent::doing(const QString& tool) {
    static const std::map<QString, QString> words = {
        {"list_media", "reading"}, {"get_project", "reading"}, {"list_looks", "reading"}, {"look_at_media", "watching"},
        {"note_media", "noting"}, {"analyze_media", "studying"}, {"get_transcript", "listening"},
        {"add_clip", "cutting"}, {"trim_clip", "cutting"}, {"move_clip", "cutting"}, {"split_clip", "cutting"}, {"delete_clips", "cutting"},
        {"set_sequence", "cutting"}, {"add_track", "cutting"}, {"set_grade", "grading"}, {"add_title", "titling"}, {"set_title_style", "titling"},
        {"search_media", "searching"}, {"set_marker", "marking"}, {"delete_markers", "marking"}, {"set_in_out", "marking"},
        {"set_transform", "editing"}, {"freeze_frame", "editing"}, {"reverse_clip", "editing"}, {"detach_audio", "editing"},
        {"duplicate_clip", "editing"}, {"add_solid", "editing"}, {"set_solid_color", "editing"}, {"set_track", "editing"},
        {"set_audio", "finishing"}, {"set_transition", "finishing"}, {"get_frame", "checking"}, {"playback", "checking"}, {"history", "checking"}};
    auto it = words.find(tool);
    return it != words.end() ? it->second : QStringLiteral("thinking");
}

void MagicAgent::start(const QString& request, Kind k) {
    if (process_) return;
    const QString exe = cli(k);
    if (exe.isEmpty()) {
        emit finished(QString(), false, QStringLiteral("%1 isn't installed on this computer.").arg(k == Kind::claude ? QStringLiteral("Claude Code") : QStringLiteral("Codex")));
        return;
    }
    // Magic needs Editroom's connection even when other apps aren't allowed in.
    if (!server_.running() && !server_.start()) {
        emit finished(QString(), false, QStringLiteral("EditroomLX couldn't open its connection for %1.").arg(name(k)));
        return;
    }
    // Nothing plays while the AI works out the edit.
    c_->model().pause();
    MagicThinking::begin(*c_, name(k), MagicShow::color(name(k)));
    kind_ = k;
    stopped_ = false;
    lastMessage_.clear();
    failure_.clear();
    pending_.clear();
    errors_.clear();
    draft_ = MCPDraft::open(c_->model().project());
    draft_->playhead = c_->model().playhead();
    const QString url = server_.endpoint() + QStringLiteral("?draft=") + draft_->token;
    // An empty folder of its own, so the AI reads no project's settings or notes.
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Editroom/Magic");
    QDir().mkpath(folder);
    QStringList args;
    if (k == Kind::claude) {
        const QString config = QStringLiteral("{\"mcpServers\":{\"editroom\":{\"type\":\"http\",\"url\":\"%1\"}}}").arg(url);
        args = {"-p", "--output-format", "stream-json", "--verbose", "--no-session-persistence",
                "--mcp-config", config, "--strict-mcp-config",
                // Editroom's tools, and web search and reading pages (to find music). No files, no shell.
                "--tools", "WebSearch,WebFetch",
                "--allowedTools", "mcp__editroom", "WebSearch", "WebFetch",
                "--disallowedTools", "mcp__editroom__auto_edit", "mcp__editroom__import_media", "mcp__editroom__save_project", "mcp__editroom__export"};
        if (!model(k).isEmpty()) args << "--model" << model(k);
        if (!effort(k).isEmpty()) args << "--effort" << effort(k);
    } else {
        args = {"--search", "exec", "--json", "--ephemeral", "--skip-git-repo-check", "--ignore-user-config",
                "-s", "read-only", "-C", folder,
                "-c", QStringLiteral("mcp_servers.editroom.url=\"%1\"").arg(url),
                "-c", "mcp_servers.editroom.tool_timeout_sec=300",
                "-c", "mcp_servers.editroom.startup_timeout_sec=30",
                "-c", "approval_policy=\"never\""};
        if (!model(k).isEmpty()) args << "-m" << model(k);
        if (!effort(k).isEmpty()) args << "-c" << QStringLiteral("model_reasoning_effort=\"%1\"").arg(effort(k));
        args << "-";
    }
    process_ = new QProcess(this);
    process_->setWorkingDirectory(folder);
    process_->setProgram(exe);
    process_->setArguments(args);
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
        pending_ += process_->readAllStandardOutput();
        qsizetype nl;
        while ((nl = pending_.indexOf('\n')) >= 0) {
            read(pending_.left(nl));
            pending_.remove(0, nl + 1);
        }
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this] { errors_ += process_->readAllStandardError().left(20000); });
    connect(process_, &QProcess::finished, this, [this](int code) { done(code); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { failure_ = QStringLiteral("Couldn't start %1.").arg(name(kind_)); done(-1); }
    });
    process_->start();
    process_->write(brief(request, c_->model().project(), MagicPointers::now(c_->model())).toUtf8());
    process_->closeWriteChannel();
    status_ = name(k) + QStringLiteral(" is thinking");
    emit statusChanged(status_);
}

void MagicAgent::stop() {
    stopped_ = true;
    if (process_) process_->terminate();
}

void MagicAgent::read(const QByteArray& line) {
    const QJsonObject j = QJsonDocument::fromJson(line).object();
    if (j.isEmpty()) return;
    if (kind_ == Kind::claude) {
        const QString type = j.value("type").toString();
        if (type == QLatin1String("result")) {
            lastMessage_ = j.value("result").toString(lastMessage_);
            if (j.value("is_error").toBool() || j.value("subtype").toString() != QLatin1String("success"))
                failure_ = lastMessage_.isEmpty() ? QStringLiteral("Claude stopped (%1).").arg(j.value("subtype").toString("error")) : lastMessage_;
        } else if (type == QLatin1String("assistant")) {
            for (const auto& c : j.value("message").toObject().value("content").toArray())
                if (c.toObject().value("type").toString() == QLatin1String("text") && !c.toObject().value("text").toString().isEmpty())
                    lastMessage_ = c.toObject().value("text").toString();
        }
    } else {
        const QString type = j.value("type").toString();
        const QJsonObject item = j.value("item").toObject();
        if (type == QLatin1String("item.completed") && item.value("type").toString() == QLatin1String("agent_message") && !item.value("text").toString().isEmpty())
            lastMessage_ = item.value("text").toString();
        else if (type == QLatin1String("turn.failed")) failure_ = j.value("error").toObject().value("message").toString(QStringLiteral("ChatGPT stopped."));
        else if (type == QLatin1String("error")) failure_ = j.value("message").toString(failure_);
    }
}

void MagicAgent::done(int code) {
    if (!process_) return;
    process_->deleteLater();
    process_ = nullptr;
    std::vector<MagicEdit> edits = draft_ ? draft_->edits : std::vector<MagicEdit>{};
    if (draft_) MCPDraft::close(draft_->token);
    draft_ = nullptr;
    status_.clear();
    emit statusChanged(status_);
    if (stopped_) { if (auto* t = MagicThinking::current()) t->cancel(); return; }
    if (edits.empty()) if (auto* t = MagicThinking::current()) t->end();
    const QString err = QString::fromUtf8(errors_).trimmed();
    if (edits.empty() && !failure_.isEmpty()) return emit finished(QString(), false, failure_.left(600));
    if (edits.empty() && code != 0) return emit finished(QString(), false, (err.isEmpty() ? QStringLiteral("It stopped (code %1).").arg(code) : err).right(600));
    if (edits.empty()) return emit finished(lastMessage_.isEmpty() ? QStringLiteral("%1 didn't change anything.").arg(name(kind_)) : lastMessage_, false, QString());
    // Played onto the timeline as it is now (in case it changed while the AI was thinking), as one step: the
    // thinking scene's sketch, then the cursors (or at once, when Magic isn't shown at work).
    MagicPlan plan{c_->model().project(), {}};
    for (size_t i = 0; i < edits.size(); ++i) plan.tasks.push_back({edits[i], i > 0 ? std::vector<int>{int(i) - 1} : std::vector<int>{}, 0});
    const QString said = lastMessage_.trimmed();
    QPointer<MagicAgent> self(this);
    auto play = [this, self, plan, said] {
        if (!self) return;
        MagicShow::play(*c_, plan, [this, self, said](bool finished) {
            if (self) emit this->finished(finished ? said : QStringLiteral("Stopped. Nothing was changed."), finished, QString());
        });
    };
    if (auto* t = MagicThinking::current()) {
        t->plan(plan.result());
        QTimer::singleShot(int(1000 * std::min(2.0, std::max(0.8, t->secondsUntilDrawn() + 0.35))), this, [play] {
            if (auto* t2 = MagicThinking::current()) t2->handOver();
            play();
        });
    } else {
        play();
    }
}

// MARK: AgentConnect

namespace {
QString codexHome() { return QDir::homePath() + QStringLiteral("/.codex"); }
QString codexConfig() { return codexHome() + QStringLiteral("/config.toml"); }
const QString codexHeader = QStringLiteral("[mcp_servers.editroom]");
bool runClaude(const QStringList& args, QString* error) {
    const QString exe = MagicAgent::cli(MagicAgent::Kind::claude);
    if (exe.isEmpty()) { if (error) *error = QStringLiteral("Claude Code isn't installed on this computer."); return false; }
    QProcess p;
    p.start(exe, args);
    if (!p.waitForFinished(30000) || p.exitCode() != 0) {
        if (error) *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}
} // namespace

bool AgentConnect::connectClaudeCode(QString* error) {
    runClaude({"mcp", "remove", "--scope", "user", "editroom"}, nullptr);
    return runClaude({"mcp", "add", "--scope", "user", "--transport", "http", "editroom", QStringLiteral("http://127.0.0.1:%1/mcp").arg(MCPServer::port)}, error);
}

bool AgentConnect::disconnectClaudeCode(QString* error) { return runClaude({"mcp", "remove", "--scope", "user", "editroom"}, error); }

bool AgentConnect::claudeCodeConnected() {
    QFile f(QDir::homePath() + QStringLiteral("/.claude.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    return QJsonDocument::fromJson(f.readAll()).object().value("mcpServers").toObject().contains(QStringLiteral("editroom"));
}

QString AgentConnect::removingEditroom(const QString& text) {
    QStringList out;
    bool skipping = false;
    for (const QString& line : text.split(u'\n')) {
        const QString t = line.trimmed();
        if (t.startsWith(u'[')) skipping = t == codexHeader || t.startsWith(QLatin1String("[mcp_servers.editroom."));
        if (!skipping) out << line;
    }
    // Drop the blank lines left at the end.
    while (!out.isEmpty() && out.last().trimmed().isEmpty()) out.removeLast();
    return out.join(u'\n');
}

QString AgentConnect::tomlString(const QString& s) {
    QString e = s;
    e.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    return u'"' + e + u'"';
}

bool AgentConnect::codexConnected() {
    QFile f(codexConfig());
    if (!f.open(QIODevice::ReadOnly)) return false;
    for (const QString& l : QString::fromUtf8(f.readAll()).split(u'\n')) if (l.trimmed() == codexHeader) return true;
    return false;
}

bool AgentConnect::connectCodex(QString* error) {
    QDir().mkpath(codexHome());
    QFile in(codexConfig());
    QString existing;
    if (in.open(QIODevice::ReadOnly)) existing = QString::fromUtf8(in.readAll());
    in.close();
    if (!existing.isEmpty()) {
        // A copy of their settings as they were, just in case.
        const QString backup = codexHome() + QStringLiteral("/config.toml.before-editroom");
        if (!QFileInfo::exists(backup)) QFile::copy(codexConfig(), backup);
    }
    QString text = removingEditroom(existing);
    if (!text.isEmpty()) text += QStringLiteral("\n\n");
    // The app itself is the go-between (`--mcp`), started by Codex.
    text += codexHeader + u'\n' + QStringLiteral("command = ") + tomlString(QCoreApplication::applicationFilePath()) + u'\n'
          + QStringLiteral("args = [\"--mcp\", \"--from\", \"codex\"]\nstartup_timeout_sec = 30\ntool_timeout_sec = 300\n");
    QFile out(codexConfig());
    if (!out.open(QIODevice::WriteOnly)) { if (error) *error = out.errorString(); return false; }
    out.write(text.toUtf8());
    return true;
}

bool AgentConnect::disconnectCodex(QString* error) {
    QFile in(codexConfig());
    if (!in.open(QIODevice::ReadOnly)) return true;
    const QString text = removingEditroom(QString::fromUtf8(in.readAll())) + u'\n';
    in.close();
    QFile out(codexConfig());
    if (!out.open(QIODevice::WriteOnly)) { if (error) *error = out.errorString(); return false; }
    out.write(text.toUtf8());
    return true;
}

} // namespace er
