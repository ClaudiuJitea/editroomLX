// InspectorView.swift's building blocks: InspectorSection, ThinSlider, ParamRow (with ArrowSteps), CurveMenu,
// Chip, MenuField, FontPicker, ColorField, FrameSizeFields, CaptionEye and CaptionTextField. Sizes, colours,
// wording and behaviour are the Mac's; each widget can be refreshed with new values from the model without
// being rebuilt (so a drag in progress isn't interrupted).
#pragma once
#include "model/Captions.h"
#include "model/Types.h"
#include <QAbstractButton>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QWidget>
#include <functional>
#include <optional>

class QVBoxLayout;
class QHBoxLayout;
class QLabel;

namespace er {

class EditorModel;

/// A titled group of rows. Click the header to fold it away; the choice is remembered per section
/// ("inspector.collapsed.<title>").
class InspectorSection : public QWidget {
    Q_OBJECT
public:
    struct Action { QString title; std::function<void()> run; };
    InspectorSection(const QString& title, std::optional<Action> action = std::nullopt, bool collapsedByDefault = false,
                     bool marked = false, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }
    void add(QWidget* w);
    void add(QLayout* l);
    bool collapsed() const { return collapsed_; }
    /// Beside the title (Grade's before/after eye); the rest of the header row still folds and unfolds.
    void setAccessory(QWidget* w);
private:
    void toggle();
    void sync();
    void placeAccessory();
    QWidget* accessory_ = nullptr;
    QString title_;
    QWidget* header_;
    QWidget* content_;
    QVBoxLayout* body_;
    QAbstractButton* action_ = nullptr;
    bool collapsed_;
    bool marked_;
    friend class SectionHeader;
};

/// Slim slider: a 2 px track, filled in the text colour, and a 13 px knob. Normalised 0…1.
class ThinSlider : public QWidget {
    Q_OBJECT
public:
    explicit ThinSlider(QWidget* parent = nullptr);
    void setValue(double n) { value_ = n; update(); }
    std::function<void(bool)> onEditing;
    std::function<void(double)> onChange;
    QSize sizeHint() const override { return {120, 20}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override { hovering_ = true; update(); }
    void leaveEvent(QEvent*) override { hovering_ = false; update(); }
private:
    void drag(double x);
    double value_ = 0;
    bool dragging_ = false, hovering_ = false;
};

/// A number field: Return or leaving it applies what was typed, Esc puts the value back, ↑/↓ step it.
class NumberField : public QLineEdit {
    Q_OBJECT
public:
    explicit NumberField(QWidget* parent = nullptr);
    std::function<void(bool up, bool big)> onStep;
    std::function<void()> onCommit;
    std::function<void()> onCancel;
signals:
    void focusChanged(bool focused);
protected:
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void paintEvent(QPaintEvent*) override;
private:
    bool cancelled_ = false;
};

/// The row icon: an SF Symbol stand-in, or the fade in/out marks.
struct RowIcon {
    enum class Kind { symbol, fadeIn, fadeOut } kind = Kind::symbol;
    QString name;
    static RowIcon symbol(const QString& n) { return {Kind::symbol, n}; }
    static RowIcon fadeIn() { return {Kind::fadeIn, {}}; }
    static RowIcon fadeOut() { return {Kind::fadeOut, {}}; }
};

class RowIconView : public QWidget {
    Q_OBJECT
public:
    explicit RowIconView(RowIcon icon, QWidget* parent = nullptr);
    void setColor(const QColor& c) { color_ = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    RowIcon icon_;
    QColor color_;
};

/// One parameter: icon, name, slider, typed value, and an optional trailing control (and one inside the
/// value field, on its right: the speed ramp and zoom buttons).
class ParamRow : public QWidget {
    Q_OBJECT
public:
    struct Spec {
        RowIcon icon;
        QString label;
        double value = 0, lo = 0, hi = 1;
        bool logarithmic = false;
        /// What ↑/↓ change it by (ten times with Shift); unset goes by what the field shows.
        std::optional<double> step;
        std::function<QString(double)> format;
        std::function<void(bool)> onEditing;
        std::function<void(double)> onChange;
        QWidget* trailing = nullptr;
        QWidget* inside = nullptr;
    };
    explicit ParamRow(Spec spec, QWidget* parent = nullptr);
    /// A new value from the model (the field keeps what's being typed in it).
    void setValue(double v);
private:
    double normalised() const;
    double denormalise(double n) const;
    void nudge(bool up, bool big);
    void commit();
    static std::optional<double> parse(const QString& s);
    Spec s_;
    ThinSlider* slider_;
    NumberField* field_;
};

/// The current fade shape as a small drawing; opens a menu of the shapes, each drawn beside its name.
class CurveMenu : public QAbstractButton {
    Q_OBJECT
public:
    CurveMenu(FadeCurve selection, bool rising, bool active, std::function<void(FadeCurve)> onPick, QWidget* parent = nullptr);
    void set(FadeCurve selection, bool active) { selection_ = selection; active_ = active; setToolTip(fadeCurveLabel(selection)); update(); }
    static QIcon glyph(FadeCurve c, bool rising, const QColor& color);
    QSize sizeHint() const override { return {48, 26}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    FadeCurve selection_;
    bool rising_, active_;
    std::function<void(FadeCurve)> onPick_;
};

/// A small toggle with a label ("Bold"): accent when on.
class Chip : public QAbstractButton {
    Q_OBJECT
public:
    Chip(const QString& label, bool active, std::function<void()> action, QWidget* parent = nullptr);
    void setActive(bool a) { active_ = a; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    bool active_;
};

/// A field that opens a menu: the current choice and a chevron.
class MenuField : public QAbstractButton {
    Q_OBJECT
public:
    explicit MenuField(const QString& label, int height = 28, QWidget* parent = nullptr);
    QMenu* menu() const { return menu_; }
    void setLabel(const QString& l) { label_ = l; update(); }
    QSize sizeHint() const override { return {160, height_}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    QString label_;
    int height_;
    QMenu* menu_;
};

/// Font family chooser: a field showing the current font that opens a searchable list, each name drawn in
/// its own face. Type to filter; Return picks the first match.
class FontPicker : public QAbstractButton {
    Q_OBJECT
public:
    FontPicker(const QString& selection, std::function<void(const QString&)> onPick, QWidget* parent = nullptr);
    void setSelection(const QString& s) { selection_ = s; update(); }
    QSize sizeHint() const override { return {160, 28}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    void open();
    QString selection_;
    std::function<void(const QString&)> onPick_;
};

/// A colour well: shows the colour, opens the colour picker (changes apply as they're picked).
class ColorField : public QAbstractButton {
    Q_OBJECT
public:
    ColorField(RGBA color, std::function<void(RGBA)> onChange, QWidget* parent = nullptr);
    void setColor(RGBA c) { color_ = c; update(); }
    QSize sizeHint() const override { return {44, 24}; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    RGBA color_;
    std::function<void(RGBA)> onChange_;
};

/// Width × height entry for a custom sequence size. Return or clicking away applies, Esc reverts, ↑/↓ two
/// pixels (twenty with Shift), since sizes are kept even.
class FrameSizeFields : public QWidget {
    Q_OBJECT
public:
    FrameSizeFields(int width, int height, std::function<void(int, int)> apply, QWidget* parent = nullptr);
    void set(int width, int height);
private:
    void sync();
    void commit();
    int width_, height_;
    std::function<void(int, int)> apply_;
    NumberField* w_;
    NumberField* h_;
};

/// Shows or hides part of the captions' look: a bare eye, a little brighter on hover.
/// Beside Grade's title: the grade on or off (off, it's kept but not shown or exported). Only there when there's a
/// grade.
class GradeEye : public QAbstractButton {
    Q_OBJECT
public:
    GradeEye(bool hidden, std::function<void()> action, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {18, 16}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    bool hidden_;
};

class CaptionEye : public QAbstractButton {
    Q_OBJECT
public:
    CaptionEye(bool on, std::function<void()> action, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {18, 16}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
private:
    bool on_;
};

/// A caption's words, retyped in place: set on Return or when the field is left; Escape puts them back.
/// Double-clicking a caption on the timeline puts the cursor here.
class CaptionTextField : public QPlainTextEdit {
    Q_OBJECT
public:
    CaptionTextField(EditorModel& m, CaptionBlock block, QWidget* parent = nullptr);
    void setBlock(const CaptionBlock& b);
    void takeRequest();
protected:
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    void commit();
    void fit();
    EditorModel& m_;
    CaptionBlock block_;
    std::optional<CaptionBlock> editing_;
    bool cancelled_ = false;
    bool settingText_ = false;
};

/// The row a colour, eye or other control sits in: icon, name, then the controls on the right.
/// `afterLabel` sits right after the name (the caption highlight and outline eyes, as Grade's beside its title).
QWidget* labelledRow(const RowIcon& icon, const QString& label, std::initializer_list<QWidget*> right, bool dim = false,
                     int labelWidth = -1, QWidget* afterLabel = nullptr);
/// Small faint note text under a row (10–10.5 px).
QLabel* note(const QString& text, double px = 10.5, int indent = 0);

/// The width of the row names' column.
int rowLabelWidth();
/// A number to a tenth ("12.5"), whole numbers without the ".0" ("12").
QString tenths(double v);
/// A percentage to a tenth ("95.3%"), whole numbers without the ".0" ("100%").
inline QString percentTenths(double v) { return tenths(v) + u'%'; }

} // namespace er
