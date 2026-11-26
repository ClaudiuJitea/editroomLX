// Theme.swift: the five looks (Auto, Dark, Grey, Amber, Light) with the exact palette values, and the
// chrome every widget paints with. Type is Inter (bundled) in place of SF Pro: 13 px body, 11.5 px small.
#pragma once
#include "model/Types.h"
#include <QColor>
#include <QFont>
#include <QString>

namespace er {

enum class AppTheme { automatic, dark, grey, amber, light };

/// Every colour a theme sets. Chrome tints (hovers, strokes, quiet fills) are `ink` at low opacity.
struct Palette {
    quint32 bg, panel, lane, laneAlt, raised, field;
    quint32 ink; double strokeAlpha, hoverAlpha;
    quint32 text, textDim, textFaint, onText, accent;
    quint32 video, audio, textClip, clipStroke, offline;
    quint32 hint, well, control, button, buttonHover, transport, outline, meterLow, chip;
    quint32 sheet, sheetField, sheetFieldHover, sheetFieldStroke, sheetLabel;
    static const Palette dark, grey, amber, light;
};

namespace Theme {
/// The settings key ("appearance") and its values ("auto", "dark", …), as on the Mac.
inline constexpr const char* key = "appearance";
AppTheme saved();
void save(AppTheme t);
QString name(AppTheme t);
QString rawValue(AppTheme t);
/// The theme being drawn (never Auto: resolved to Light or Dark by the desktop's colour scheme).
AppTheme current();
void setCurrent(AppTheme t);
bool isLight();
const Palette& p();

QColor hex(quint32 h, double alpha = 1);
QColor bg(); QColor panel(); QColor lane(); QColor laneAlt(); QColor raised(); QColor field();
QColor ink(double alpha = 1); QColor stroke(); QColor hover();
QColor text(); QColor textDim(); QColor textFaint(); QColor onText(); QColor accent(); QColor hint();
/// A neutral grey for the row under the pointer in menus and for selected text.
QColor highlight();
QColor well(); QColor control(); QColor button(); QColor buttonHover(); QColor transport(); QColor outline();
QColor selection();
/// Too loud: the same red-orange in every theme; meter yellow.
QColor alert(); QColor snap();
QColor clip(ClipKind kind);
/// With filmstrips off: pictures blue, audio green, text plum, solids ochre.
QColor clipType(ClipKind kind);
QColor clipStroke(); QColor offline();

/// Inter at a pixel size and weight.
QFont font(double px = 13, int weight = 400);
QFont mono(double px, int weight = 500);
/// The small caps labels ("MEDIA", "VIDEO FADE"): 10 px semibold.
QFont label(double px = 10);
void loadFonts();
/// The application-wide style sheet for the current palette.
QString styleSheet();
/// The theme on the application: its palette (backgrounds, text, highlight), font and style sheet.
void apply();
} // namespace Theme

} // namespace er
