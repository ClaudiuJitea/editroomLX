// The CAPTIONS check from SelfTest.swift (layout, following the clip, fixes, removal), without the drawing
// part, which comes with the compositor.
#include "model/Captions.h"
#include <QTest>
#include <cmath>

using namespace er;

class TestCaptions : public QObject {
    Q_OBJECT

    static QStringList texts(const std::vector<CaptionBlock>& b) { QStringList s; for (auto& x : b) s << x.text(); return s; }

private slots:
    void captions() {
        MediaAsset video;
        video.path = QStringLiteral("/media/a_main.mp4"); video.name = QStringLiteral("a_main.mp4");
        video.kind = MediaKind::video; video.duration = 10; video.width = 1280; video.height = 720; video.fps = 30; video.hasAudio = true;
        Project p;
        p.width = 1280; p.height = 720; p.fps = 30;
        p.assets = {video};
        const Uuid v1 = p.videoTracks[0].id, a1 = p.audioTracks[0].id;
        const Uuid link = newUuid();
        Clip cv(v1, ClipKind::video, 5, 3, 1); cv.assetID = video.id; cv.linkID = link;
        Clip ca(a1, ClipKind::audio, 5, 3, 1); ca.assetID = video.id; ca.linkID = link;
        p.clips = {cv, ca};
        const std::vector<std::tuple<QString, double, double>> said = {
            {"hello", 0.2, 0.5}, {"so", 1.1, 1.3}, {"here's", 1.35, 1.6}, {"the", 1.62, 1.7}, {"thing.", 1.72, 2.0},
            {"we", 2.8, 2.95}, {"made", 3.0, 3.3}, {"it", 3.35, 3.5}, {"again", 4.5, 4.8}};
        Transcript script;
        script.language = QStringLiteral("en");
        for (auto& [t, s, e] : said) script.words.push_back({s, e, t});
        auto heard = [&](const MediaAsset&) -> std::optional<Transcript> { return script; };

        // Captioning the video means its sound.
        const IdSet targets = p.captionTargets({cv.id});
        QCOMPARE(targets, IdSet{ca.id});
        p.addCaptions(targets);
        auto blocks = CaptionLayout::blocks(p, heard);
        QCOMPARE(texts(blocks), (QStringList{"so here's the thing.", "we made it"}));
        QVERIFY(blocks.size() == 2 && std::abs(blocks[0].start - 5.1) < 0.001 && std::abs(blocks[1].start - 6.8) < 0.001);
        for (auto& b : blocks) QVERIFY(b.end <= 8 + 1e-9);

        // Moved 2s later and played at 200%: each word where it's now said.
        for (auto& c : p.clips) { c.start += 2; c.speed = 2; c.duration = 1.5; }
        auto moved = CaptionLayout::blocks(p, heard);
        QVERIFY(moved.size() == 2 && std::abs(moved[0].start - (7 + 0.05)) < 0.001 && std::abs(moved[1].start - (7 + 0.9)) < 0.001);
        for (auto& c : p.clips) { c.start -= 2; c.speed = 1; c.duration = 3; }

        // A fix (same words count, one for one), and a caption left out.
        blocks = CaptionLayout::blocks(p, heard);
        p.setCaptionText(blocks[1], QStringLiteral("we made it!"));
        p.setCaptionText(blocks[0], QString());
        QCOMPARE(texts(CaptionLayout::blocks(p, heard)), QStringList{"we made it!"});

        p.removeCaptions({ca.id});
        QVERIFY(!p.captions && !p.captionFixes);
        for (auto& c : p.clips) QVERIFY(c.captioned != true);

        // Deleting every captioned clip takes the caption track with it.
        Project emptied = p;
        emptied.addCaptions({ca.id});
        emptied.clips.clear();
        emptied.sanitize();
        QVERIFY(!emptied.captions);

        // Captioned again: every word is back, the earlier edits gone.
        p.addCaptions({ca.id});
        QCOMPARE(texts(CaptionLayout::blocks(p, heard)), (QStringList{"so here's the thing.", "we made it"}));

        // Typing: fewer words (the first takes them all), then the same words again.
        blocks = CaptionLayout::blocks(p, heard);
        p.setCaptionText(blocks[1], QStringLiteral("we did"));
        QCOMPARE(CaptionLayout::blocks(p, heard).back().text(), QStringLiteral("we did"));
        // More words than were said: the caption keeps its time, the words share it.
        p.setCaptionText(blocks[1], QStringLiteral("and then we finally made it work"));
        std::vector<CaptionBlock> longer;
        for (auto& b : CaptionLayout::blocks(p, heard)) if (b.start >= blocks[1].start - 1e-9) longer.push_back(b);
        QCOMPARE(texts(longer).join(u' '), QStringLiteral("and then we finally made it work"));
        QVERIFY(std::abs(longer.front().start - blocks[1].start) < 1e-6 && std::abs(longer.back().end - blocks[1].end) < 1e-6);
        size_t words = 0;
        for (auto& b : longer) words += b.words.size();
        QCOMPARE(words, size_t(7));
        p.setCaptionText(blocks[1], QStringLiteral("we made it"));
        QCOMPARE(CaptionLayout::blocks(p, heard).back().text(), QStringLiteral("we made it"));
    }

    void blockId() {
        CaptionBlock b;
        b.clipID = QUuid::fromString(QStringLiteral("6f1d8c64-1b2e-4c38-9a6e-0b9c1f6a1b2c"));
        b.words = {{0, 1, "x", 1.2345, {1.2345}}};
        QCOMPARE(b.id(), QStringLiteral("6F1D8C64-1B2E-4C38-9A6E-0B9C1F6A1B2C@1234"));
    }

    void graphemes() {
        QCOMPARE(graphemeCount(QStringLiteral("café")), 4);
        QCOMPARE(graphemeCount(QString::fromUtf8("👍🏽")), 1);
    }
};

QTEST_GUILESS_MAIN(TestCaptions)
#include "tst_captions.moc"
