#include "Captions.h"
#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>

namespace er {

int graphemeCount(const QString& s) {
    QTextBoundaryFinder f(QTextBoundaryFinder::Grapheme, s);
    int n = 0;
    while (f.toNextBoundary() != -1) ++n;
    return n;
}

static QStringList splitWhitespace(const QString& s) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return s.split(ws, Qt::SkipEmptyParts);
}

QString CaptionBlock::id() const {
    const double first = words.empty() ? 0 : words.front().source;
    return clipID.toString(QUuid::WithoutBraces).toUpper() + u'@' + QString::number(qint64(first * 1000));
}

QString CaptionBlock::text() const {
    QStringList parts;
    for (const auto& w : words) parts << w.text;
    return parts.join(u' ');
}

std::vector<double> CaptionBlock::covered() const {
    std::vector<double> out;
    for (const auto& w : words)
        for (double c : w.covers)
            if ((out.empty() || out.back() != c) && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    return out;
}

// MARK: Project caption edits

std::vector<Clip> Project::captionedClips() const {
    if (!captions) return {};
    std::vector<Clip> out;
    for (const auto& c : clips)
        if (c.captioned == true && c.kind == ClipKind::audio && c.playsSource() && !c.playsBackwards()) out.push_back(c);
    return out;
}

IdSet Project::captionTargets(const IdSet& ids) const {
    const IdSet group = linked(ids);
    IdSet out;
    for (const auto& c : clips) if (group.count(c.id) && c.kind == ClipKind::audio && c.playsSource()) out.insert(c.id);
    return out;
}

IdSet Project::defaultCaptionTargets() const {
    std::set<Uuid> voices;
    for (const auto& t : audioTracks) if (t.duckUnder) voices.insert(*t.duckUnder);
    IdSet out;
    for (const auto& c : clips) {
        if (c.kind != ClipKind::audio || !c.playsSource()) continue;
        const MediaAsset* a = asset(c.assetID);
        if (!a) continue;
        if (a->kind == MediaKind::video || voices.count(c.trackID)) out.insert(c.id);
    }
    return out;
}

void Project::addCaptions(const IdSet& ids) {
    if (!captions) captions = CaptionTrack();
    for (auto& c : clips) if (ids.count(c.id)) c.captioned = true;
}

void Project::removeCaptions(const IdSet& ids) {
    for (auto& c : clips) if (ids.count(c.id)) c.captioned.reset();
    if (!std::any_of(clips.begin(), clips.end(), [](const Clip& c) { return c.captioned == true; })) return removeCaptionTrack();
    std::set<Uuid> still;
    for (const auto& c : clips) if (c.captioned == true && c.assetID) still.insert(*c.assetID);
    if (captionFixes) {
        std::erase_if(*captionFixes, [&](const auto& kv) { return !still.count(kv.first); });
        if (captionFixes->empty()) captionFixes.reset();
    }
}

void Project::removeCaptionTrack() {
    captions.reset();
    captionFixes.reset();
    for (auto& c : clips) c.captioned.reset();
}

void Project::setCaptionText(const CaptionBlock& block, const QString& text) {
    const QStringList fresh = splitWhitespace(text);
    const std::vector<double> said = block.covered();
    std::vector<CaptionFix> fixes;
    if (captionFixes) if (auto it = captionFixes->find(block.assetID); it != captionFixes->end()) fixes = it->second;
    auto set = [&](double source, const QString& t, bool merged = false) {
        std::erase_if(fixes, [&](const CaptionFix& f) { return std::abs(f.start - source) < 0.0005; });
        CaptionFix f;
        f.start = source;
        f.text = t;
        if (merged) f.merged = true;
        fixes.push_back(f);
    };
    if (fresh.isEmpty()) {
        for (double s : said) set(s, QString());
    } else if (size_t(fresh.size()) == said.size()) {
        // Every word set, so words an earlier edit left out or joined come back.
        for (size_t i = 0; i < said.size(); ++i) set(said[i], fresh[qsizetype(i)]);
    } else {
        for (size_t i = 0; i < said.size(); ++i) set(said[i], i == 0 ? fresh.join(u' ') : QString(), i > 0);
    }
    if (!captionFixes) captionFixes.emplace();
    std::stable_sort(fixes.begin(), fixes.end(), [](const CaptionFix& a, const CaptionFix& b) { return a.start < b.start; });
    (*captionFixes)[block.assetID] = fixes;
}

// MARK: Layout

namespace {
using Word = CaptionBlock::Word;

bool endsSentence(const QString& t) { return t.endsWith(u'.') || t.endsWith(u'?') || t.endsWith(u'!'); }

/// A run of talking cut into even phrases: as many as keep each within `maxChars` and `maxSeconds`,
/// each cut at the word nearest an even share (a little nearer after a comma).
std::vector<std::vector<Word>> phrases(const std::vector<Word>& run) {
    if (run.empty()) return {};
    const Word& first = run.front();
    const Word& last = run.back();
    // Characters up to the end of each word.
    std::vector<int> ends;
    int total = 0;
    for (size_t i = 0; i < run.size(); ++i) {
        total += graphemeCount(run[i].text) + (i > 0 ? 1 : 0);
        ends.push_back(total);
    }
    const int byChars = int(std::ceil(double(total) / double(CaptionLayout::maxChars)));
    const int byTime = int(std::ceil((last.end - first.start) / CaptionLayout::maxSeconds));
    const int n = std::min(int(run.size()), std::max({1, byChars, byTime}));
    if (!(n > 1)) return {run};
    std::vector<int> cuts;
    int from = 0;
    for (int k = 1; k < n; ++k) {
        const double target = double(total) * double(k) / double(n);
        // Cut after word i, leaving a word for each phrase to come. Never inside a retyped word's new
        // words: they're one edit.
        std::optional<int> best;
        double bestScore = INFINITY;
        const int upper = int(run.size()) - (n - k) - 1;
        for (int i = from; i <= upper; ++i) {
            if (run[i].source == run[i + 1].source) continue;
            double score = std::abs(double(ends[i]) - target);
            if (run[i].text.endsWith(u',')) score -= 6;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        if (!best) break;
        cuts.push_back(*best);
        from = *best + 1;
        if (from > int(run.size()) - 1) break;
    }
    std::vector<std::vector<Word>> out;
    int start = 0;
    cuts.push_back(int(run.size()) - 1);
    for (int c : cuts) {
        out.emplace_back(run.begin() + start, run.begin() + c + 1);
        start = c + 1;
    }
    return out;
}

std::vector<CaptionBlock> blocksFor(const Clip& clip, const MediaAsset& asset, const std::vector<Transcript::Word>& words,
                                    const std::vector<CaptionFix>& fixes) {
    const double s0 = clip.sourceStart, s1 = s0 + clip.sourceLength();
    auto at = [&](double s) { return std::min(clip.end(), std::max(clip.start, clip.start + clip.timelineSpan(s0, s))); };
    // The words heard in the clip (most of the word inside it), on the timeline, with any fixes. A word
    // joined to the one before lends it its time.
    std::vector<Word> entries;
    std::optional<double> previous;
    for (const auto& w : words) {
        const double mid = (w.start + w.end) / 2;
        if (!(mid >= s0 && mid < s1)) continue;
        const CaptionFix* fix = nullptr;
        for (const auto& f : fixes) if (std::abs(f.start - w.start) < 0.0005) { fix = &f; break; }
        const double a = at(std::max(w.start, s0)), b = at(std::min(w.end, s1));
        if (fix && fix->merged == true) {
            if (previous && !entries.empty() && !entries.back().covers.empty() && entries.back().covers.back() == *previous) {
                entries.back().end = std::max(entries.back().end, b);
                entries.back().covers.push_back(w.start);
            }
            previous = w.start;
            continue;
        }
        const QString text = fix ? fix->text : w.text;
        // Swift trims `.whitespaces` (spaces and tabs) only.
        QString trimmed = text;
        while (!trimmed.isEmpty() && (trimmed.front() == u' ' || trimmed.front() == u'\t')) trimmed.remove(0, 1);
        while (!trimmed.isEmpty() && (trimmed.back() == u' ' || trimmed.back() == u'\t')) trimmed.chop(1);
        if (trimmed.isEmpty() || !(b > a)) { previous = w.start; continue; }
        entries.push_back(Word{a, b, text, w.start, {w.start}});
        previous = w.start;
    }
    // A retyped word with several words in it: they share its time, by length.
    std::vector<Word> heard;
    for (const auto& e : entries) {
        const QStringList parts = splitWhitespace(e.text);
        if (!(parts.size() > 1)) { heard.push_back(e); continue; }
        std::vector<double> weights;
        double total = 0;
        for (const auto& p : parts) { weights.push_back(double(graphemeCount(p) + 1)); total += weights.back(); }
        double t = e.start;
        for (qsizetype i = 0; i < parts.size(); ++i) {
            const double d = (e.end - e.start) * weights[size_t(i)] / total;
            heard.push_back(Word{t, t + d, parts[i], e.source, e.covers});
            t += d;
        }
    }
    // Runs of talking: broken by a pause or the end of a sentence.
    std::vector<std::vector<Word>> runs;
    for (const auto& w : heard) {
        if (!runs.empty() && !runs.back().empty()) {
            const Word& lastW = runs.back().back();
            if (w.start - lastW.end < CaptionLayout::pause && !endsSentence(lastW.text)) { runs.back().push_back(w); continue; }
        }
        runs.push_back({w});
    }
    std::vector<std::vector<Word>> groups;
    for (const auto& r : runs) for (auto& p : phrases(r)) groups.push_back(std::move(p));

    std::vector<CaptionBlock> out;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (g.empty()) continue;
        const Word& first = g.front();
        const Word& lastW = g.back();
        // Held a moment after the last word, never into the next caption or past the clip.
        const double next = i + 1 < groups.size() ? groups[i + 1][0].start : clip.end();
        const double end = std::min({next, clip.end(), std::max(lastW.end + 0.25, first.start + 0.6)});
        out.push_back(CaptionBlock{clip.id, asset.id, first.start, std::max(end, lastW.end), g});
    }
    return out;
}
} // namespace

std::vector<CaptionBlock> CaptionLayout::blocks(const Project& project, const TranscriptFor& transcript) {
    std::vector<CaptionBlock> out;
    for (const auto& clip : project.captionedClips()) {
        const MediaAsset* asset = project.asset(clip.assetID);
        if (!asset) continue;
        auto script = transcript(*asset);
        if (!script || script->isEmpty()) continue;
        std::vector<CaptionFix> fixes;
        if (project.captionFixes) if (auto it = project.captionFixes->find(asset->id); it != project.captionFixes->end()) fixes = it->second;
        auto b = blocksFor(clip, *asset, script->words, fixes);
        out.insert(out.end(), b.begin(), b.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const CaptionBlock& a, const CaptionBlock& b) { return a.start < b.start; });
    // Overlapping speech (two people at once): each caption gives way to the next.
    for (size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i].end > out[i + 1].start) out[i].end = std::max(out[i].start + 0.05, out[i + 1].start);
    return out;
}

} // namespace er
