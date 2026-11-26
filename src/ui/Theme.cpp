#include "Theme.h"
#include <QCoreApplication>
#include <QDir>
#include <cmath>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

namespace er {

const Palette Palette::dark{
    0x0A0A0A, 0x111111, 0x0D0D0D, 0x0A0A0A, 0x1A1A1A, 0x181818,
    0xFFFFFF, 0.085, 0.07,
    0xF2F2F0, 0x9A9A96, 0x5C5C59, 0x0A0A0A, 0xFF4A1C,
    0x1E1E1E, 0x151515, 0x1B1B1B, 0x2C2C2C, 0x5A1F1F,
    0x3E3E3C, 0x262626, 0x3A3A3A, 0x242424, 0x2E2E2E, 0x161616, 0x2C2C2C, 0xD9D9D6, 0xC9C9C5,
    0x141414, 0x1E1E1E, 0x262626, 0x2E2E2E, 0xA8A8A4};
const Palette Palette::grey{
    0x1C1C1C, 0x232323, 0x1F1F1F, 0x1C1C1C, 0x2D2D2D, 0x2A2A2A,
    0xFFFFFF, 0.09, 0.07,
    // (Grey has no orange: its accent, for switches, the playhead and what's active, is a mid grey.)
    0xF2F2F0, 0xA6A6A2, 0x6F6F6C, 0x1C1C1C, 0x9A9A96,
    0x333333, 0x2A2A2A, 0x303030, 0x444444, 0x6A2A2A,
    0x585856, 0x383838, 0x4C4C4C, 0x363636, 0x404040, 0x2A2A2A, 0x444444, 0xD9D9D6, 0xCFCFCB,
    0x262626, 0x303030, 0x383838, 0x424242, 0xAEAEAA};
const Palette Palette::amber{
    0x100B06, 0x18120A, 0x130E08, 0x100B06, 0x261C10, 0x22190E,
    0xFFE8CC, 0.09, 0.075,
    0xF7E7CF, 0xB39B7C, 0x76634C, 0x140E07, 0xFFA43A,
    0x2E2316, 0x21180D, 0x2A1F13, 0x45341F, 0x5E2414,
    0x5A4830, 0x352816, 0x523F27, 0x33261A, 0x403021, 0x21190F, 0x3E3022, 0xE6D6BD, 0xD8C6AA,
    0x1A140C, 0x261D13, 0x30251A, 0x3A2D1F, 0xB8A182};
const Palette Palette::light{
    0xE3E3E0, 0xF4F4F2, 0xEAEAE7, 0xE3E3E0, 0xFFFFFF, 0xFFFFFF,
    0x000000, 0.11, 0.055,
    0x1C1C1B, 0x5F5F5B, 0x98988F, 0xFFFFFF, 0xF0451A,
    0x6A6A67, 0x73736F, 0x6A6A67, 0x5A5A57, 0x9A3A30,
    0x9E9E99, 0xD5D5D1, 0xC6C6C1, 0xE4E4E1, 0xDADAD6, 0xE6E6E3, 0xC4C4BF, 0x8C8C87, 0x3A3A38,
    0xF4F4F2, 0xFFFFFF, 0xF8F8F6, 0xD3D3CE, 0x5F5F5B};

namespace {
AppTheme currentTheme = AppTheme::dark;
QString fontFamily = QStringLiteral("Inter");
}

QString Theme::rawValue(AppTheme t) {
    switch (t) {
    case AppTheme::automatic: return QStringLiteral("auto");
    case AppTheme::dark: return QStringLiteral("dark");
    case AppTheme::grey: return QStringLiteral("grey");
    case AppTheme::amber: return QStringLiteral("amber");
    case AppTheme::light: return QStringLiteral("light");
    }
    return {};
}

QString Theme::name(AppTheme t) {
    const QString r = rawValue(t);
    return r.left(1).toUpper() + r.mid(1);
}

AppTheme Theme::saved() {
    const QString v = QSettings().value(QLatin1String(key)).toString();
    for (AppTheme t : {AppTheme::automatic, AppTheme::dark, AppTheme::grey, AppTheme::amber, AppTheme::light})
        if (rawValue(t) == v) return t;
    // Grey until chosen otherwise (Settings › General › Theme).
    return AppTheme::grey;
}

void Theme::save(AppTheme t) { QSettings().setValue(QLatin1String(key), rawValue(t)); }

static AppTheme resolve(AppTheme t) {
    if (t != AppTheme::automatic) return t;
    const bool dark = qGuiApp && qGuiApp->styleHints()->colorScheme() != Qt::ColorScheme::Light;
    return dark ? AppTheme::dark : AppTheme::light;
}

AppTheme Theme::current() { return currentTheme; }
void Theme::setCurrent(AppTheme t) { currentTheme = resolve(t); }
bool Theme::isLight() { return currentTheme == AppTheme::light; }

const Palette& Theme::p() {
    switch (currentTheme) {
    case AppTheme::grey: return Palette::grey;
    case AppTheme::amber: return Palette::amber;
    case AppTheme::light: return Palette::light;
    default: return Palette::dark;
    }
}

QColor Theme::hex(quint32 h, double a) { QColor c(QRgb(0xFF000000 | h)); c.setAlphaF(float(a)); return c; }
QColor Theme::bg() { return hex(p().bg); }
QColor Theme::panel() { return hex(p().panel); }
QColor Theme::lane() { return hex(p().lane); }
QColor Theme::laneAlt() { return hex(p().laneAlt); }
QColor Theme::raised() { return hex(p().raised); }
QColor Theme::field() { return hex(p().field); }
QColor Theme::ink(double a) { return hex(p().ink, a); }
QColor Theme::stroke() { return ink(p().strokeAlpha); }
QColor Theme::hover() { return ink(p().hoverAlpha); }
QColor Theme::text() { return hex(p().text); }
QColor Theme::textDim() { return hex(p().textDim); }
QColor Theme::textFaint() { return hex(p().textFaint); }
QColor Theme::onText() { return hex(p().onText); }
QColor Theme::accent() { return hex(p().accent); }
QColor Theme::hint() { return hex(p().hint); }
QColor Theme::well() { return hex(p().well); }
QColor Theme::control() { return hex(p().control); }
QColor Theme::button() { return hex(p().button); }
QColor Theme::buttonHover() { return hex(p().buttonHover); }
QColor Theme::transport() { return hex(p().transport); }
QColor Theme::outline() { return hex(p().outline); }
QColor Theme::highlight() {
    switch (currentTheme) {
    case AppTheme::light: return hex(0xD6D6D2);
    case AppTheme::amber: return hex(0x45341F);
    case AppTheme::grey: return hex(0x4A4A4A);
    default: return hex(0x3A3A3A);
    }
}

QColor Theme::selection() { return isLight() ? hex(0x1C1C1B) : QColor(Qt::white); }
QColor Theme::alert() { return hex(0xFF4A1C); }
QColor Theme::snap() { return hex(0xF2C14E); }
QColor Theme::clipStroke() { return hex(p().clipStroke); }
QColor Theme::offline() { return hex(p().offline); }

QColor Theme::clip(ClipKind k) {
    switch (k) {
    case ClipKind::video: case ClipKind::image: return hex(p().video);
    case ClipKind::audio: return hex(p().audio);
    case ClipKind::text: case ClipKind::solid: return hex(p().textClip);
    }
    return hex(p().video);
}

QColor Theme::clipType(ClipKind k) {
    switch (k) {
    case ClipKind::video: case ClipKind::image: return hex(0x3A5A80);
    case ClipKind::audio: return hex(0x2F6A47);
    case ClipKind::text: return hex(0x664675);
    case ClipKind::solid: return hex(0x7A6428);
    }
    return hex(0x3A5A80);
}

void Theme::loadFonts() {
    const QStringList dirs = {QCoreApplication::applicationDirPath() + QStringLiteral("/../share/editroom/fonts"),
                              QStringLiteral(ER_RESOURCES "/fonts")};
    for (const QString& d : dirs) {
        for (const QString& f : QDir(d).entryList({QStringLiteral("*.ttf")})) {
            const int id = QFontDatabase::addApplicationFont(d + u'/' + f);
            if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()) fontFamily = QFontDatabase::applicationFontFamilies(id).first();
        }
        if (!QDir(d).entryList({QStringLiteral("*.ttf")}).isEmpty()) break;
    }
}

QFont Theme::font(double px, int weight) {
    QFont f(fontFamily);
    f.setPixelSize(int(std::round(px)));
    if (px != std::round(px)) f.setPointSizeF(px * 0.75); // 11.5 px
    f.setWeight(QFont::Weight(weight));
    return f;
}

QFont Theme::mono(double px, int weight) {
    QFont f = font(px, weight);
    f.setFeature(QFont::Tag("tnum"), 1); // monospaced digits, as SF's monospacedDigit
    return f;
}

QFont Theme::label(double px) {
    QFont f = font(px, 600);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    return f;
}

void Theme::apply() {
    QPalette pal = QApplication::palette();
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        pal.setColor(group, QPalette::Window, panel());
        pal.setColor(group, QPalette::Base, field());
        pal.setColor(group, QPalette::AlternateBase, raised());
        pal.setColor(group, QPalette::Button, button());
        pal.setColor(group, QPalette::WindowText, text());
        pal.setColor(group, QPalette::Text, text());
        pal.setColor(group, QPalette::ButtonText, text());
        pal.setColor(group, QPalette::Highlight, highlight());
        pal.setColor(group, QPalette::HighlightedText, text());
        pal.setColor(group, QPalette::ToolTipBase, raised());
        pal.setColor(group, QPalette::ToolTipText, text());
        pal.setColor(group, QPalette::PlaceholderText, textFaint());
    }
    pal.setColor(QPalette::Disabled, QPalette::WindowText, textFaint());
    pal.setColor(QPalette::Disabled, QPalette::Text, textFaint());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, textFaint());
    QApplication::setPalette(pal);
    QApplication::setFont(font(13));
    qApp->setStyleSheet(styleSheet());
}

QString Theme::styleSheet() {
    auto c = [](const QColor& col) { return col.name(col.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb); };
    QString s;
    // Only the text colour for every widget: a font or background here would override each widget's own (labels'
    // sizes, the cards in Settings), so the defaults come from the application's palette and font instead (apply()).
    s += QStringLiteral("QWidget { color: %1; }\n").arg(c(text()));
    s += QStringLiteral("QMainWindow, QDialog { background: %1; }\n").arg(c(bg()));
    s += QStringLiteral("QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 4px 6px; border-radius: 5px; }\n").arg(c(raised()), c(text()), c(stroke()));
    // Menus as the desktop draws them: a grey panel, a lighter grey rounded row under the pointer, the text as it is,
    // at 13.5 px (10.1 pt: style sheets take only whole pixels).
    s += QStringLiteral("QMenuBar { background: %1; font-size: 10.1pt; padding: 2px 4px; }"
                        "QMenuBar::item { padding: 4px 10px; border-radius: 6px; background: transparent; }"
                        "QMenuBar::item:selected, QMenuBar::item:pressed { background: %2; }\n").arg(c(panel()), c(highlight()));
    s += QStringLiteral("QMenu { background: %1; border: 1px solid %2; padding: 5px; border-radius: 10px; font-size: 10.1pt; }"
                        "QMenu::item { padding: 6px 28px 6px 12px; border-radius: 6px; background: transparent; }"
                        "QMenu::item:selected { background: %3; color: %5; }"
                        "QMenu::item:disabled { color: %4; } QMenu::separator { height: 1px; background: %2; margin: 6px 10px; }\n")
             .arg(c(raised()), c(stroke()), c(highlight()), c(textFaint()), c(text()));
    s += QStringLiteral("QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: %1; border: 1px solid %2; border-radius: 6px; padding: 3px 6px; selection-background-color: %3; selection-color: %4; }\n")
             .arg(c(field()), c(stroke()), c(highlight()), c(text()));
    s += QStringLiteral("QComboBox QAbstractItemView { background: %1; border: 1px solid %2; selection-background-color: %3; selection-color: %4; }\n")
             .arg(c(raised()), c(stroke()), c(highlight()), c(text()));
    s += QStringLiteral("QPushButton { background: %1; border: none; border-radius: 13px; padding: 5px 14px; font-weight: 600; }"
                        "QPushButton:hover { background: %2; } QPushButton:disabled { color: %3; }\n").arg(c(button()), c(buttonHover()), c(textFaint()));
    s += QStringLiteral("QPushButton[primary=\"true\"] { background: %1; color: %2; } QPushButton[primary=\"true\"]:hover { background: %3; }\n").arg(c(text()), c(onText()), c(textDim()));
    s += QStringLiteral("QSlider::groove:horizontal { height: 3px; background: %1; border-radius: 1px; }"
                        "QSlider::sub-page:horizontal { background: %2; border-radius: 1px; }"
                        "QSlider::handle:horizontal { background: %3; width: 13px; height: 13px; margin: -5px 0; border-radius: 6px; }\n")
             .arg(c(well()), c(textDim()), c(text()));
    s += QStringLiteral("QScrollBar:vertical { background: transparent; width: 8px; } QScrollBar::handle:vertical { background: %1; border-radius: 4px; min-height: 30px; }"
                        "QScrollBar:horizontal { background: transparent; height: 8px; } QScrollBar::handle:horizontal { background: %1; border-radius: 4px; min-width: 30px; }"
                        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; } QScrollBar::add-page, QScrollBar::sub-page { background: none; }\n").arg(c(control()));
    s += QStringLiteral("QSplitter::handle { background: %1; } QTabBar::tab { background: transparent; padding: 6px 12px; color: %2; }"
                        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %4; } QTabWidget::pane { border: none; }\n")
             .arg(c(stroke()), c(textDim()), c(text()), c(text()));
    s += QStringLiteral("QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; border: 1px solid %1; background: %2; } QCheckBox::indicator:checked { background: %3; border-color: %3; }\n")
             .arg(c(control()), c(field()), c(textDim()));
    return s;
}

} // namespace er
