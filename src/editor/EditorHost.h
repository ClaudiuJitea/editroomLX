// What EditorModel needs from the rest of the app: the player, the media store, transcripts and dialogs.
// The Mac's EditorModel owns these directly (PlaybackEngine, MediaStore, NSAlert); LX keeps the model
// testable without them, and the app implements this interface. Every function has a harmless default.
#pragma once
#include "model/Project.h"
#include <QStringList>

namespace er {

class EditorHost {
public:
    virtual ~EditorHost() = default;

    // Player (PlaybackEngine)
    virtual void rebuild(const Project&, double /*playhead*/) {}
    virtual void seek(double /*t*/, bool /*fast*/ = false) {}
    virtual void play() {}
    virtual void pause() {}
    virtual double currentTime() const { return 0; }
    virtual bool engineEmpty() const { return false; }
    /// A frame from the made-ahead scrub frames; false when there isn't one.
    virtual bool showScrubFrame(double /*t*/) { return false; }
    virtual void endScrubFrames() {}
    virtual void setLoopSpan(std::optional<Range>) {}
    virtual void setAudible(const std::map<Uuid, bool>&) {}
    virtual void scrubSound(double /*t*/) {}
    /// Audio scrubbing turned on or off: the scrub mix is made (or let go) ahead of the first scrub.
    virtual void audioScrubChanged(bool /*on*/) {}

    // Media (MediaStore, Transcripts, AudioLevels)
    virtual std::optional<Transcript> transcript(const Uuid& /*asset*/) const { return std::nullopt; }
    virtual void makeTranscript(const Uuid& /*asset*/, bool /*soon*/) {}
    virtual bool transcriptsAvailable() const { return true; }
    /// Captions or a transcript are wanted and the speech model isn't here: offers to download it. True when the
    /// download has started (captions then wait for it).
    virtual bool offerSpeechModel() { return false; }
    /// A file's transcript is queued or being made.
    virtual bool transcriptPending(const Uuid& /*asset*/) const { return false; }
    virtual void prepareReversedAudio(const Uuid& /*asset*/) {}
    virtual std::optional<double> envelopeDB(const Clip&) const { return std::nullopt; }
    virtual std::optional<double> peakDB(const Clip&) const { return std::nullopt; }
    virtual void expect(const std::vector<MediaAsset>&) {}

    // Dialogs (EditroomAlert): the index of the button pressed, buttons in the Mac's order.
    virtual int alert(const QString& /*message*/, const QString& /*info*/, const QStringList& buttons) {
        return buttons.isEmpty() ? 0 : 0;
    }
    virtual void showError(const QString&) {}
    /// Asks where a missing item's file is (`thenOthers`: keep asking for whatever is still missing after).
    virtual void locate(const Uuid& /*asset*/, bool /*thenOthers*/) {}
    /// Asks where a linked folder that's gone missing is now.
    virtual void locateFolder(const Uuid& /*folder*/) {}
    virtual QString chooseSaveFile(const QString& /*suggestedName*/) { return {}; }
    virtual QString chooseProjectFile() { return {}; }
};

} // namespace er
