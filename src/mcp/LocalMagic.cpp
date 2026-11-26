#include "LocalMagic.h"
#include "LocalImprover.h"
#include "LocalPlanners.h"
#include "MagicShow.h"
#include "MagicThinking.h"
#include "ui/Controller.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <mutex>

namespace er {

LocalMagic& LocalMagic::of(Controller& c) {
    if (auto* l = c.findChild<LocalMagic*>(QString(), Qt::FindDirectChildrenOnly)) return *l;
    return *new LocalMagic(c);
}

LocalMagic::LocalMagic(Controller& c) : QObject(&c), c_(c) { pool_.setMaxThreadCount(3); }

LocalMagic::~LocalMagic() {
    ++run_;
    pool_.waitForDone();
}

void LocalMagic::setStatus(const QString& s) {
    status_ = s;
    emit statusChanged();
}

void LocalMagic::finish(const QString& said, bool changed) {
    if (captionsAfter_) {
        captionsAfter_ = false;
        if (changed) { busy_ = false; captionCut(said); return; }
    }
    busy_ = false;
    setStatus(QString());
    // Nothing to play (or stopped): the thinking scene fades where it is.
    if (auto* t = MagicThinking::current()) t->end();
    emit finished(said, changed);
}

void LocalMagic::scene() {
    if (MagicThinking::current()) return;
    MagicThinking::begin(c_, QStringLiteral("Local"), QColor::fromRgbF(0.95, 0.95, 0.95));
    sceneClock_.start();
}

void LocalMagic::play(const MagicPlan& plan, std::function<void(bool)> done) {
    MagicThinking* thinking = MagicThinking::current();
    if (!thinking) { MagicShow::play(c_, plan, std::move(done)); return; }
    // Long enough to see the scouting (cached analysis is instant), then the timeline laid out for the finished cut
    // and the cut sketched on it; a beat to take it in, and the cursors set off as the scene fades around them.
    const int run = run_;
    QPointer<LocalMagic> self(this);
    const int wait = std::max(0, int(2400 - (sceneClock_.isValid() ? sceneClock_.elapsed() : 2400)));
    QTimer::singleShot(wait, this, [this, self, run, plan, done] {
        MagicThinking* t = MagicThinking::current();
        if (!self || run != run_) return;
        if (!t) { MagicShow::play(c_, plan, done); return; }
        EditorModel& m = c_.model();
        const Project result = plan.result();
        m.live([&](Project& q) { q = result; }, false);
        m.zoomToFit();
        m.discardLive();
        t->plan(result);
        const int hold = int(1000 * std::min(2.0, std::max(0.8, t->secondsUntilDrawn() + 0.35)));
        QTimer::singleShot(hold, this, [this, self, run, plan, done] {
            if (!self || run != run_) return;
            if (auto* t2 = MagicThinking::current()) t2->handOver();
            MagicShow::play(c_, plan, done);
        });
    });
}

void LocalMagic::stop() {
    MagicShow::stopAll();
    if (auto* t = MagicThinking::current()) t->cancel();
    if (!busy_) return;
    ++run_;
    busy_ = false;
    setStatus(QString());
}

QString LocalMagic::madeNote(const EditDecision& d, double length, const LocalRecipe& recipe) {
    QStringList names;
    for (const auto& s : d.shots) if (!names.contains(s.asset.name)) names << s.asset.name;
    const QString of = recipe.asked.isEmpty() ? QString() : QStringLiteral(" of ") + (recipe.asked.startsWith(QLatin1String("the ")) ? QString() : QStringLiteral("the ")) + recipe.asked;
    const int n = int(d.shots.size());
    const QString clips = n == 1 ? QStringLiteral("1 clip") : QStringLiteral("%1 clips").arg(n);
    QString from;
    if (names.size() == 1) {
        from = QStringLiteral(" from ") + names[0];
    } else if (names.size() <= 3 || ((!recipe.asked.isEmpty() || !recipe.keepOnly.empty()) && names.size() <= 5)) {
        from = QStringLiteral(" from ") + names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + names.last();
    } else {
        const bool videos = std::all_of(d.shots.begin(), d.shots.end(), [](const auto& s) { return s.asset.kind == MediaKind::video; });
        from = QStringLiteral(" from %1 %2 in your Media").arg(names.size()).arg(videos ? QStringLiteral("videos") : QStringLiteral("files"));
    }
    return QStringLiteral("Made a %1-second cut").arg(int(std::round(length))) + of + QStringLiteral(": ") + clips + from
         + (d.music ? QStringLiteral(", cut to the beat") : QString()) + u'.';
}

void LocalMagic::start(const LocalRecipe& recipe, std::function<void(QString, bool)> done) {
    if (busy_) return;
    auto say = [this, done](const QString& said, bool changed) {
        if (done) done(said, changed);
        finish(said, changed);
    };
    EditorModel& model = c_.model();
    const Project project = model.project();
    std::set<Uuid> offline;
    for (const auto& a : project.assets) if (c_.media().isOffline(a)) offline.insert(a.id);
    // With an edit on the timeline, Magic improves that edit rather than starting over from the whole library.
    std::set<Uuid> onTimeline;
    for (const auto& c : project.clips) if ((c.kind == ClipKind::video || c.kind == ClipKind::image) && c.assetID) onTimeline.insert(*c.assetID);
    const bool improving = !onTimeline.empty() && !recipe.wholeLibrary;
    std::vector<MediaAsset> visuals;
    for (const auto& a : project.assets) {
        if (offline.count(a.id)) continue;
        if (!(a.kind == MediaKind::image || (a.kind == MediaKind::video && a.duration >= 1))) continue;
        if (improving && !onTimeline.count(a.id)) continue;
        if (!recipe.keepOnly.empty() && !recipe.keepOnly.count(a.id)) continue;
        if (recipe.takeOut.count(a.id)) continue;
        visuals.push_back(a);
    }
    if (visuals.empty()) {
        busy_ = true;
        if (recipe.isFocused())
            say(QStringLiteral("%1 %2 in your %3, so there was nothing to keep.")
                    .arg(recipe.mediaNames.join(QStringLiteral(", ")), recipe.mediaNames.size() == 1 ? QStringLiteral("isn't") : QStringLiteral("aren't"),
                         improving ? QStringLiteral("edit") : QStringLiteral("Media")),
                false);
        else
            say(QStringLiteral("Magic needs videos or photos in your Media to make a cut from."), false);
        return;
    }
    // The song: the one the edit uses, or (for a new cut) the longest in the library (8 seconds or more).
    std::optional<MediaAsset> songOnTimeline, librarySong;
    for (const auto& c : project.clips) {
        if (c.kind != ClipKind::audio || c.linkID) continue;
        const MediaAsset* a = project.asset(c.assetID);
        if (a && a->kind == MediaKind::audio && a->duration >= 8 && (!songOnTimeline || a->duration > songOnTimeline->duration)) songOnTimeline = *a;
    }
    for (const auto& a : project.assets)
        if (a.kind == MediaKind::audio && !offline.count(a.id) && a.duration >= 8 && (!librarySong || a.duration > librarySong->duration)) librarySong = a;
    const std::optional<MediaAsset> newSong = improving && recipe.music == LocalRecipe::Music::add && !songOnTimeline ? librarySong : std::nullopt;
    std::optional<MediaAsset> song;
    if (recipe.music != LocalRecipe::Music::off) {
        if (improving) song = newSong ? newSong : (songOnTimeline && !offline.count(songOnTimeline->id) ? songOnTimeline : std::nullopt);
        else song = librarySong;
    }
    busy_ = true;
    const int run = ++run_;
    // The thinking scene (one already running, for a request being read, carries on): looking, then analysing.
    scene();
    {
        std::set<Uuid> ids;
        for (const auto& a : visuals) ids.insert(a.id);
        QTimer::singleShot(700, this, [this, run, ids] {
            if (run != run_) return;
            if (auto* t = MagicThinking::current()) t->analyze(ids);
        });
    }
    setStatus(QStringLiteral("Analyzing 0 of %1").arg(visuals.size() + (song ? 1 : 0)));

    // Analysed a few at a time, off the main thread.
    struct Found {
        std::mutex lock;
        std::map<Uuid, ClipAnalysis> clips;
        std::optional<MusicAnalysis> music;
        int done = 0, total = 0;
        /// Planned once, by whichever finished analysis is seen last on the main thread.
        bool planned = false;
    };
    auto found = std::make_shared<Found>();
    found->total = int(visuals.size()) + (song ? 1 : 0);
    QPointer<LocalMagic> self(this);
    auto finished = [this, self, run, found, visuals, song, improving, newSong, recipe, say] {
        if (!self || run != run_) return;
        setStatus(QString());
        std::vector<std::pair<MediaAsset, ClipAnalysis>> clips;
        for (const auto& a : visuals) if (auto it = found->clips.find(a.id); it != found->clips.end()) clips.push_back({a, it->second});
        std::optional<std::pair<MediaAsset, MusicAnalysis>> music;
        if (song && found->music) music = std::make_pair(*song, *found->music);
        EditorModel& model = c_.model();
        std::optional<MagicPlan> plan;
        QString said;
        QStringList notes;
        if (!recipe.cannotDo.isEmpty()) notes << recipe.cannotDo;
        if (improving) {
            std::map<Uuid, ClipAnalysis> analyses;
            for (const auto& [a, an] : clips) analyses.emplace(a.id, an);
            const auto outcome = LocalImprover::plan(model.project(), analyses, music ? std::optional<MusicAnalysis>(music->second) : std::nullopt, recipe, newSong);
            if (outcome && !outcome->plan.tasks.empty()) plan = outcome->plan;
            if (!outcome) said = QStringLiteral("Your edit already looks good; nothing to change.");
            else if (outcome->message) said = *outcome->message;
            else said = outcome->focused ? outcome->summary.left(1).toUpper() + outcome->summary.mid(1) + u'.'
                                         : QStringLiteral("Improved your edit: %1.").arg(outcome->summary);
            if (outcome && !outcome->message) notes += outcome->notes;
        } else {
            const auto decision = AutoPlanner::decide(model.project(), clips, music, recipe);
            if (decision) plan = Choreographer::plan(model.project(), *decision);
            if (decision) {
                double length = 0;
                for (const auto& s : decision->shots) length += s.duration;
                said = madeNote(*decision, length, recipe);
                if (recipe.seconds && length < *recipe.seconds - 1)
                    notes << QStringLiteral("your Media only had enough good footage for about %1 seconds").arg(int(std::round(length)));
                if (recipe.every) {
                    std::set<Uuid> used;
                    for (const auto& s : decision->shots) used.insert(s.asset.id);
                    if (used.size() < clips.size()) notes << QStringLiteral("%1 of your clips were too short to use").arg(clips.size() - used.size());
                    if (recipe.seconds && length > *recipe.seconds + 0.5) notes << QStringLiteral("fitting every clip in made it %1 seconds").arg(int(std::round(length)));
                }
            }
            // The frame shape they asked for, from the start.
            std::optional<std::pair<int, int>> size;
            switch (recipe.frame) {
            case LocalRecipe::Frame::vertical: size = std::make_pair(1080, 1920); break;
            case LocalRecipe::Frame::horizontal: size = std::make_pair(1920, 1080); break;
            case LocalRecipe::Frame::square: size = std::make_pair(1080, 1080); break;
            case LocalRecipe::Frame::unchanged: break;
            }
            if (size && plan) {
                plan->base.width = size->first;
                plan->base.height = size->second;
                plan->base.settingsLocked = true;
            }
        }
        if (!notes.isEmpty()) said += QStringLiteral(" It couldn't do everything you asked: ") + notes.join(QStringLiteral("; ")) + u'.';
        if (!plan) {
            say(said.isEmpty() ? QStringLiteral("Magic couldn't find anything to make a cut from.") : said, false);
            return;
        }
        // Played by the cursors (or made at once): one undo step.
        setStatus(QString());
        play(*plan, [say, said](bool finished) {
            say(finished ? said : QStringLiteral("Stopped. Nothing was changed."), finished);
        });
    };
    auto progress = [this, self, run, found] {
        if (!self || run != run_) return;
        int done;
        {
            std::lock_guard g(found->lock);
            done = found->done;
        }
        setStatus(done < found->total ? QStringLiteral("Analyzing %1 of %2").arg(done).arg(found->total) : QString());
    };
    auto oneDone = [self, found, progress, finished] {
        QMetaObject::invokeMethod(self.data(), [found, progress, finished] {
            progress();
            int done;
            {
                std::lock_guard g(found->lock);
                done = found->done;
            }
            if (done >= found->total && !found->planned) {
                found->planned = true;
                finished();
            }
        }, Qt::QueuedConnection);
    };
    if (song) {
        const QString path = c_.media().readPath(*song);
        pool_.start([found, path, oneDone] {
            auto m = MediaAnalyzer::music(path);
            { std::lock_guard g(found->lock); found->music = m; ++found->done; }
            oneDone();
        });
    }
    for (const auto& a : visuals) {
        const QString path = c_.media().readPath(a);
        const MediaRuntime* rt = c_.media().runtime(a.id);
        const int orientation = rt ? rt->orientation : 1;
        const bool image = a.kind == MediaKind::image;
        const Uuid id = a.id;
        pool_.start([found, path, image, orientation, id, oneDone] {
            auto an = MediaAnalyzer::clip(path, image, orientation);
            { std::lock_guard g(found->lock); if (an) found->clips[id] = *an; ++found->done; }
            oneDone();
        });
    }
}

void LocalMagic::hear(const std::vector<Uuid>& assets, std::function<void()> then) {
    // What's said in these files, heard first (each up to a minute); without the speech model, straight on.
    std::vector<QString> paths;
    for (const auto& id : assets) {
        const QString path = c_.media().soundPath(id);
        if (!path.isEmpty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    }
    if (paths.empty() || !Transcripts::available()) { then(); return; }
    setStatus(QStringLiteral("Listening"));
    auto left = std::make_shared<int>(int(paths.size()));
    const int run = run_;
    QPointer<LocalMagic> self(this);
    for (const auto& path : paths)
        c_.transcripts().ready(path, [this, self, left, run, then](std::optional<Transcript>) {
            if (!self || run != run_ || --*left > 0) return;
            setStatus(QString());
            then();
        }, 60);
}

void LocalMagic::perform(const LocalOutcome& outcome) {
    if (!outcome.plan || outcome.plan->tasks.empty()) { finish(outcome.said, false); return; }
    const QString said = outcome.said;
    play(*outcome.plan, [this, said](bool finished) {
        finish(finished ? said : QStringLiteral("Stopped. Nothing was changed."), finished);
    });
}

void LocalMagic::run(const QString& typedWithMarkers) {
    if (busy_) return;
    EditorModel& model = c_.model();
    // Markers named with @ are read as their times; @mentions in plain words for the reader.
    const QString typed = MagicMention::markersAsTimes(typedWithMarkers.trimmed(), model.project());
    const MagicPointers pointers = MagicPointers::now(model);
    const Project p = model.project();
    std::set<Uuid> pointed;
    for (const auto& f : MagicMention::find(typed, p.assets)) if (f.mention.kind == MagicMention::Kind::media) pointed.insert(f.mention.id);
    const QString r = MagicMention::plain(typed, p.assets, MagicMention::clipLabels(p)).trimmed();
    // No request: Local's own improvement (or a new cut).
    if (r.isEmpty()) return start(LocalRecipe{});
    busy_ = true;
    ++run_;
    scene();
    auto transcriptOf = [this](const Uuid& id) { return c_.media().transcript(id); };
    auto assetsOf = [&](const IdSet& clips) {
        std::vector<Uuid> out;
        for (const auto& c : p.clips) if (clips.count(c.id) && c.assetID) out.push_back(*c.assetID);
        return out;
    };
    // Pauses in the talking: from what's said.
    if (LocalSpeech::asks(r)) {
        const auto found = MagicMention::find(typed, p.assets, MagicMention::clipLabels(p));
        IdSet only;
        bool selected = typed.toLower().contains(QLatin1String("selected"));
        for (const auto& f : found) {
            if (f.mention.kind == MagicMention::Kind::clip) only.insert(f.mention.id);
            if (f.mention.kind == MagicMention::Kind::selected) selected = true;
        }
        if (selected) only.insert(pointers.selected.begin(), pointers.selected.end());
        IdSet videos;
        for (const auto& c : p.clips) if (c.kind == ClipKind::video) videos.insert(c.id);
        hear(assetsOf(videos), [this, only, transcriptOf] {
            perform(LocalSpeech::plan(c_.model().project(), only.empty() ? std::nullopt : std::optional<IdSet>(only), transcriptOf));
        });
        return;
    }
    // A zoom on a shot ("@selected add a zoom", "push in"): before adding clips, so "add" here isn't a new clip.
    if (LocalZoom::asks(r)) {
        const LocalOutcome outcome = LocalZoom::plan(typed, pointers, p);
        if (QCoreApplication::arguments().contains(QStringLiteral("--magic-local")))
            fprintf(stderr, "MAGIC LOCAL zoom: %s (%d edits)\n", qPrintable(outcome.said), outcome.plan ? int(outcome.plan->tasks.size()) : 0);
        return perform(outcome);
    }
    // More than one thing asked: all of them, in turn.
    const auto parts = LocalCombined::parts(r, p);
    if (parts.size() > 1) {
        std::vector<Uuid> toHear;
        if (std::find(parts.begin(), parts.end(), LocalCombined::Part::captions) != parts.end() && !LocalBrain::captionsRemoved(r))
            toHear = assetsOf(LocalCaptions::targets(pointers, p));
        hear(toHear, [this, parts, r, typed, pointers, transcriptOf] {
            perform(LocalCombined::plan(parts, r, typed, pointers, c_.model().project(), transcriptOf));
        });
        return;
    }
    if (LocalCaptions::asks(r, p)) {
        const auto toHear = LocalBrain::captionsRemoved(r) ? std::vector<Uuid>{} : assetsOf(LocalCaptions::targets(pointers, p));
        hear(toHear, [this, r, pointers, transcriptOf] { perform(LocalCaptions::plan(r, pointers, c_.model().project(), transcriptOf)); });
        return;
    }
    if (LocalAdd::asks(r, p)) return perform(LocalAdd::plan(r, typed, pointers, p));
    if (LocalTitle::asks(r, p)) return perform(LocalTitle::plan(r, pointers.playhead, p));
    if (LocalRamp::asks(r)) return perform(LocalRamp::plan(typed, pointers, p));

    const bool hasEdit = std::any_of(p.clips.begin(), p.clips.end(), [](const Clip& c) { return c.kind == ClipKind::video || c.kind == ClipKind::image; });
    LocalRecipe recipe = LocalBrain::recipe(r, p.assets, pointed, hasEdit, [this](const Uuid& id) { return c_.sceneTags(id); });
    if (recipe.levelAudio) {
        // Every audio clip on a track that's on, measured now, so they all read the same on the output meters.
        std::vector<Clip> clips;
        for (const auto& c : p.clips) {
            const Track* t = p.track(c.trackID);
            if (c.kind == ClipKind::audio && !p.isLocked(c.trackID) && !(t && t->muted)) clips.push_back(c);
        }
        recipe.levelGains = model.levelGains(clips, true);
        std::set<QString> silent;
        for (const auto& c : clips) if (!recipe.levelGains.count(c.id)) if (const MediaAsset* a = p.asset(c.assetID)) silent.insert(a->name);
        recipe.levelSilent = QStringList(silent.begin(), silent.end());
    }
    // About the selected clips, the playhead or the in/out range: just that, directly.
    if (auto pointedEdit = LocalPointed::plan(typed, recipe, pointers, p)) return perform(*pointedEdit);
    // Nothing in it Local can do (and it isn't just "make it better"): say so, edit nothing.
    if (recipe.isEmpty() && !LocalBrain::isGeneral(r)) {
        const QString what = recipe.cannotDo.isEmpty() ? QStringLiteral("that") : recipe.cannotDo;
        const bool question = r.contains(u'?') && recipe.cannotDo.isEmpty();
        finish(question ? QStringLiteral("Local only edits your video, so it left it as it was. To ask a question, use Claude or ChatGPT.")
                        : QStringLiteral("Local can't do %1. It can change the length, the pace, the look, the music, the frame shape (vertical, square), "
                                         "and which files to use or take out, by name. For anything that needs understanding what's in your shots, "
                                         "use Claude or ChatGPT.").arg(what),
               false);
        return;
    }
    busy_ = false;
    // Captions asked for with the cut: put on the cut once it's made, in the same undo step.
    captionsAfter_ = LocalBrain::captionsWithCut(r);
    start(recipe);
}

void LocalMagic::captionCut(const QString& said) {
    const Project& p = c_.model().project();
    const IdSet targets = p.unlocked(p.defaultCaptionTargets());
    if (targets.empty()) { emitNote(said + QStringLiteral(" No one talks in the cut, so there are no captions."), true); return; }
    std::vector<Uuid> assets;
    for (const auto& c : p.clips) if (targets.count(c.id) && c.assetID) assets.push_back(*c.assetID);
    busy_ = true;
    hear(assets, [this, said, targets] {
        c_.model().amendLastStep([&](Project& q) { q.addCaptions(targets); });
        const int count = int(c_.model().captionBlocks().size());
        finish(said + u' ' + (count == 0 ? QStringLiteral("Added a caption track, but no speech was heard in the cut.")
                                         : QStringLiteral("Captioned what's said: %1 caption%2.").arg(count).arg(count == 1 ? QString() : QStringLiteral("s"))),
               true);
    });
}

void LocalMagic::emitNote(const QString& said, bool changed) { finish(said, changed); }

} // namespace er
