// InspectorView.swift: what's picked, and its settings, under a header naming it.
//   Nothing picked: the sequence (Frame, Frame rate).
//   Captions picked, or the caption track's header: the caption's words and the captions' Style.
//   Clips: Text or Solid first, then Playback (Volume, Speed with its ramp graph), Transform (Scale with its
//   zoom graph, X, Y), Transitions (In, Out), the fades (with their curves) and Grade (the three wheels,
//   Exposure, Contrast, Saturate, Temp, Tint, Look, Intensity).
// Rebuilt when what it shows changes shape; otherwise its rows are refreshed in place, so a drag or typing
// in progress carries on. Sliders are live changes (one undo step per drag).
#pragma once
#include "Controller.h"
#include <QWidget>
#include <functional>

class QVBoxLayout;
class QScrollArea;
class QLabel;

namespace er {

class InspectorSection;

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int width = 320;
    explicit InspectorPanel(Controller& c, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void changed();
    void rebuild();
    QByteArray signature() const;
    std::vector<Clip> selected() const;
    bool showsCaptions() const;
    QString title(const std::vector<Clip>& clips) const;
    QString headerSymbol(const std::vector<Clip>& clips) const;

    void captionSection(QVBoxLayout* v);
    QWidget* captionColorRow(const QString& label, const QString& icon, std::optional<bool> on,
                             std::function<RGBA(const CaptionStyle&)> get, std::function<void(CaptionStyle&)> toggle,
                             std::function<void(CaptionStyle&, RGBA)> set);
    void sequenceSection(QVBoxLayout* v);
    void clipSections(QVBoxLayout* v, const std::vector<Clip>& clips);
    void toggleRamp(const Clip& clip, const IdSet& ids);
    void applySpeed(double speed, const IdSet& ids);
    void transformSection(QVBoxLayout* v, const std::vector<Clip>& clips);
    void transitionSection(QVBoxLayout* v, const Clip& c);
    void transitionRows(InspectorSection* s, const QString& label, const Uuid& into, const Uuid& from);
    void fadeSection(QVBoxLayout* v, const QString& title, const std::vector<Clip>& clips);
    void colorSection(QVBoxLayout* v, const std::vector<Clip>& clips);
    std::optional<QString> chooseCube();
    void textSection(QVBoxLayout* v, const Clip& c);
    void textTransformSection(QVBoxLayout* v, const Clip& c);

    /// A drag or nudge began or ended: the panel isn't rebuilt while one is under way.
    void editing(bool began);
    /// Called after every change to keep the rows showing the model's values.
    void onRefresh(std::function<void()> f) { refreshers_.push_back(std::move(f)); }

    EditorModel& model() { return c_.model(); }
    const EditorModel& model() const { return c_.model(); }

    Controller& c_;
    QWidget* header_;
    QLabel* headerTitle_;
    QString headerIcon_;
    QScrollArea* scroll_;
    QWidget* body_ = nullptr;
    QByteArray shown_;
    std::vector<std::function<void()>> refreshers_;
    int editing_ = 0;
    bool pending_ = false;
    std::optional<std::vector<Clip>> speedOrigin_, rampOrigin_;
    /// Where the playhead was before a ramp or zoom dot's drag moved it to the dot; put back when the drag ends.
    std::optional<double> rampPlayhead_, zoomPlayhead_;
};

} // namespace er
