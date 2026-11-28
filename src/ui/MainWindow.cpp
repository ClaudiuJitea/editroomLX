#include "MainWindow.h"
#include <QAbstractSpinBox>
#include <QTextEdit>
#include "Updates.h"
#include <QIcon>
#include <QDialog>
#include "Alert.h"
#include "ExportDialog.h"
#include "InspectorPanel.h"
#include "LibraryPanel.h"
#include "PreviewPanel.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "TimelineCanvas.h"
#include "TimelinePanel.h"
#include "Widgets.h"
#include "TabStrip.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace er {

MainWindow::MainWindow(Workspace& w, QWidget* parent)
    : QMainWindow(parent), w_(w),
      keys_([] { return QSettings().value(QLatin1String(ShortcutStore::key)).toByteArray(); },
            [](const std::optional<QByteArray>& d) { QSettings s; if (d) s.setValue(QLatin1String(ShortcutStore::key), *d); else s.remove(QLatin1String(ShortcutStore::key)); }) {
    setWindowTitle(QStringLiteral("EditroomLX"));
    setMinimumSize(1080, 680);
    resize(1480, 940);
    setAcceptDrops(true);
    // Every tab's editor asks its questions (alerts, file dialogs) over this window.
    w_.setUp = [this](Controller& c) { c.setWindow(this); };
    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    // Title strip: the tabs, then the export pill, Magic and Export.
    auto* strip = new QWidget;
    strip_ = strip;
    strip->setFixedHeight(38);
    auto* sh = new QHBoxLayout(strip);
    sh->setContentsMargins(10, 0, 10, 0);
    sh->setSpacing(0);
    auto* tabs = new TabStrip(w_, keys_);
    tabs_ = tabs;
    sh->addWidget(tabs, 1);
    sh->addSpacing(14);
    exportPill_ = new QLabel;
    exportPill_->setFont(Theme::font(11, 600));
    exportPill_->hide();
    sh->addWidget(exportPill_);
    auto* magic = new PillButton(QStringLiteral("Magic"));
    magic->setObjectName(QStringLiteral("magicButton"));
    magic->setToolTip(QStringLiteral("Magic"));
    connect(magic, &QPushButton::clicked, this, &MainWindow::magicRequested);
    sh->addWidget(magic);
    sh->addSpacing(8);
    magicButton_ = magic;
    // Off in Settings › Magic: the Magic button goes.
    magic->setVisible(QSettings().value(QStringLiteral("magicEnabled"), true).toBool());
    auto* exp = new PillButton(QStringLiteral("Export"), true);
    exp->setToolTip(QStringLiteral("Export (%1)").arg(keys_.combos(ShortcutAction::export_).empty() ? QStringLiteral("Ctrl+E") : keys_.combos(ShortcutAction::export_).front().display()));
    connect(exp, &QPushButton::clicked, this, &MainWindow::showExport);
    sh->addWidget(exp);
    v->addWidget(strip);
    v->addWidget(new Hairline(Qt::Horizontal));
    setCentralWidget(central);
    connect(&w_, &Workspace::aboutToSwitch, this, &MainWindow::tearDownEditor);
    connect(&w_, &Workspace::activeChanged, this, [this] { buildEditor(); emit controllerChanged(*c_); });
    connect(&w_, &Workspace::tabsChanged, this, [this] { refreshTitle(); rebuildMenus(); });
    qApp->installEventFilter(this);
    buildEditor();
}

void MainWindow::tearDownEditor() {
    if (!editor_) return;
    // The views go before their editor goes behind: the viewer lets go of its window surface first.
    if (c_ && c_->model().viewerOnly()) c_->model().toggleViewerFullScreen();
    editor_->hide();
    delete editor_;
    editor_ = nullptr;
    library_ = nullptr;
    preview_ = nullptr;
    inspector_ = nullptr;
    timeline_ = nullptr;
    top_ = nullptr;
    vertical_ = nullptr;
    exportPill_->hide();
}

void MainWindow::buildEditor() {
    tearDownEditor();
    c_ = &w_.active();
    editor_ = new QWidget;
    auto* ev = new QVBoxLayout(editor_);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(0);
    // Panels.
    library_ = new LibraryPanel(*c_);
    preview_ = new PreviewPanel(*c_);
    inspector_ = new InspectorPanel(*c_);
    timeline_ = new TimelinePanel(*c_);
    library_->setMinimumWidth(190);
    library_->setMaximumWidth(440);
    inspector_->setFixedWidth(InspectorPanel::width);
    top_ = new QSplitter(Qt::Horizontal);
    top_->addWidget(library_);
    top_->addWidget(preview_);
    top_->addWidget(inspector_);
    top_->setStretchFactor(1, 1);
    top_->setHandleWidth(1);
    top_->setChildrenCollapsible(false);
    vertical_ = new QSplitter(Qt::Vertical);
    vertical_->addWidget(top_);
    vertical_->addWidget(timeline_);
    vertical_->setHandleWidth(1);
    vertical_->setChildrenCollapsible(false);
    timeline_->setMinimumHeight(170);
    ev->addWidget(vertical_, 1);
    static_cast<QVBoxLayout*>(centralWidget()->layout())->addWidget(editor_, 1);
    // The window starts on the timeline, so the Media search never takes focus as Editroom opens.
    timeline_->canvas()->setFocus(Qt::OtherFocusReason);
    // The panels' sizes, the same in every tab.
    QSettings s;
    const int libW = s.value(QStringLiteral("libraryWidth"), 252).toInt();
    const int tlH = s.value(QStringLiteral("timelineHeight"), 360).toInt();
    const int W = std::max(width(), 1080), H = std::max(height(), 680);
    top_->setSizes({libW, W - libW - InspectorPanel::width, InspectorPanel::width});
    vertical_->setSizes({H - 38 - tlH, tlH});
    connect(top_, &QSplitter::splitterMoved, this, [this] { QSettings().setValue(QStringLiteral("libraryWidth"), top_->sizes().value(0)); });
    connect(vertical_, &QSplitter::splitterMoved, this, [this] { QSettings().setValue(QStringLiteral("timelineHeight"), vertical_->sizes().value(1)); });
    // Connections to this tab's editor go with its views.
    QWidget* ctx = editor_;
    connect(&c_->model(), &EditorModel::changed, ctx, [this] { refreshTitle(); });
    connect(&c_->model(), &EditorModel::viewerOnlyChanged, ctx, [this](bool on) { setViewerOnly(on); });
    connect(&c_->model(), &EditorModel::selectionChanged, ctx, [this] { rebuildMenus(); });
    connect(&c_->model(), &EditorModel::changed, ctx, [this] { rebuildMenus(); });
    auto pill = [this] {
        const auto* cur = c_->exports().current();
        if (!cur) { exportPill_->hide(); return; }
        QString t = QStringLiteral("Exporting %1%").arg(int(c_->exports().progress() * 100));
        if (c_->exports().waiting()) t += QStringLiteral("  +%1").arg(c_->exports().waiting());
        exportPill_->setText(t);
        exportPill_->show();
    };
    connect(&c_->exports(), &ExportQueue::changed, ctx, pill);
    connect(&c_->exports(), &ExportQueue::finished, ctx, [this](const QString& path, bool ok, const QString& msg) {
        if (!ok && !msg.isEmpty()) Alert::run(this, QStringLiteral("Couldn't export “%1”").arg(QFileInfo(path).fileName()), msg, {QStringLiteral("OK")});
    });
    pill();
    refreshTitle();
    rebuildMenus();
}

void MainWindow::applyTheme() {
    Theme::apply();
    for (QWidget* w : findChildren<QWidget*>()) w->update();
}

void MainWindow::refreshTitle() {
    if (!c_) return;
    tabs_->update();
    setWindowTitle(c_->model().title() + QStringLiteral(" — EditroomLX"));
}

void MainWindow::rebuildMenus() {
    if (!c_) return;
    EditorModel& m = c_->model();
    menuBar()->clear();
    auto item = [this](QMenu* menu, const QString& title, std::function<void()> act, std::optional<ShortcutAction> a = {}, bool enabled = true) {
        QAction* x = menu->addAction(title);
        x->setEnabled(enabled);
        if (a) {
            // The menu shows the first of an action's shortcuts; plain Backspace, Space, arrows and the like
            // stay with the editor window (KeyCommands), so a dialog's Backspace never deletes clips.
            if (auto k = keys_.menu(*a)) {
                x->setShortcut(QKeySequence(k->display()));
                // Ctrl+Shift+[ arrives as Ctrl+Shift+{ (and ] as }) on most layouts: both work.
                if (k->shift && (k->key == QLatin1String("[") || k->key == QLatin1String("]"))) {
                    KeyCombo braced = *k;
                    braced.key = k->key == QLatin1String("[") ? QStringLiteral("{") : QStringLiteral("}");
                    x->setShortcuts({QKeySequence(k->display()), QKeySequence(braced.display())});
                }
                x->setShortcutContext(Qt::WindowShortcut);
                if (k->typesText()) x->setShortcutVisibleInContextMenu(true);
                if (k->typesText()) x->setShortcut(QKeySequence()), x->setText(title + u'\t' + k->display());
            }
        }
        connect(x, &QAction::triggered, this, act);
        return x;
    };
    // File
    QMenu* file = menuBar()->addMenu(QStringLiteral("File"));
    item(file, QStringLiteral("New"), [&m] { m.newProject(); }, ShortcutAction::new_);
    item(file, QStringLiteral("New Tab"), [this] { w_.newTab(); }, ShortcutAction::newTab);
    item(file, QStringLiteral("Open…"), [&m] { m.openProject(); }, ShortcutAction::open);
    QMenu* recent = file->addMenu(QStringLiteral("Open Recent"));
    const QStringList recents = m.recentProjects();
    for (const QString& r : recents) recent->addAction(QFileInfo(r).completeBaseName(), this, [&m, r] { m.openRecent(r); });
    if (!recents.isEmpty()) recent->addSeparator();
    recent->addAction(QStringLiteral("Clear Menu"), this, [&m] { m.clearRecents(); })->setEnabled(!recents.isEmpty());
    file->addSeparator();
    item(file, QStringLiteral("Close Tab"), [this] { w_.closeActive(); }, ShortcutAction::closeTab, w_.canClose());
    item(file, QStringLiteral("Save"), [&m] { m.save(); }, ShortcutAction::save);
    item(file, QStringLiteral("Save As…"), [&m] { m.saveAs(); }, ShortcutAction::saveAs);
    file->addSeparator();
    item(file, QStringLiteral("Import Media…"), [this] { c_->showImportPanel(); }, ShortcutAction::importMedia);
    item(file, QStringLiteral("Export…"), [this] { showExport(); }, ShortcutAction::export_);
    file->addSeparator();
    QAction* quit = file->addAction(QStringLiteral("Quit EditroomLX"), this, &QWidget::close);
    quit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
    // Edit
    QMenu* edit = menuBar()->addMenu(QStringLiteral("Edit"));
    item(edit, QStringLiteral("Undo"), [&m] { m.undo(); }, ShortcutAction::undo, m.canUndo());
    item(edit, QStringLiteral("Redo"), [&m] { m.redo(); }, ShortcutAction::redo, m.canRedo());
    edit->addSeparator();
    auto textAction = [&](const QString& t, const char* slot, QKeySequence::StandardKey k) {
        QAction* a = edit->addAction(t, this, [slot] {
            if (QWidget* f = QApplication::focusWidget()) QMetaObject::invokeMethod(f, slot);
        });
        a->setShortcut(k);
    };
    textAction(QStringLiteral("Cut"), "cut", QKeySequence::Cut);
    textAction(QStringLiteral("Copy"), "copy", QKeySequence::Copy);
    textAction(QStringLiteral("Paste"), "paste", QKeySequence::Paste);
    {
        // Select All: the text in a text field; anywhere else, every clip on the timeline, and the timeline has the
        // keys now (out of Media, say), so Delete reaches the clips.
        QAction* a = edit->addAction(QStringLiteral("Select All"), this, [this] {
            QWidget* f = QApplication::focusWidget();
            if (f && (qobject_cast<QLineEdit*>(f) || qobject_cast<QTextEdit*>(f) || qobject_cast<QPlainTextEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f))) {
                QMetaObject::invokeMethod(f, "selectAll");
                return;
            }
            c_->model().selectAll();
            timeline_->canvas()->setFocus(Qt::ShortcutFocusReason);
        });
        a->setShortcut(QKeySequence::SelectAll);
    }
    edit->addSeparator();
    QAction* settings = edit->addAction(QStringLiteral("Settings…"), this, &MainWindow::showSettings);
    settings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    // Clip
    QMenu* clip = menuBar()->addMenu(QStringLiteral("Clip"));
    item(clip, QStringLiteral("Split at Playhead"), [&m] { m.splitAtPlayhead(); }, ShortcutAction::split);
    item(clip, QStringLiteral("Add Dissolve"), [&m] { m.addTransitionAtPlayhead(); }, ShortcutAction::addDissolve, m.canAddDissolve());
    item(clip, QStringLiteral("Even Out Audio Levels"), [&m] { m.evenOutLevels(); });
    item(clip, QStringLiteral("Detach Audio"), [&m] { m.detachAudio(); }, ShortcutAction::detachAudio, m.selectionIsLinked());
    item(clip, QStringLiteral("Duplicate"), [&m] { m.duplicateSelection(); }, ShortcutAction::duplicate, m.canDuplicate());
    item(clip, QStringLiteral("Freeze Frame"), [&m] { m.freezeFrame(); }, ShortcutAction::freezeFrame, m.canFreezeFrame());
    item(clip, QStringLiteral("Reverse"), [&m] { m.reverseSelection(); }, ShortcutAction::reverse, m.canReverse());
    clip->addSeparator();
    item(clip, QStringLiteral("Copy Grade"), [&m] { m.copyGrade(); }, ShortcutAction::copyGrade, m.canCopyGrade());
    item(clip, QStringLiteral("Paste Grade"), [&m] { m.pasteGrade(); }, ShortcutAction::pasteGrade, m.canPasteGrade());
    clip->addSeparator();
    const MediaAsset* matchable = m.matchableAsset();
    item(clip, QStringLiteral("Match Sequence to Clip"), [&m] { if (auto* a = m.matchableAsset()) m.matchSequence(*a); }, std::nullopt,
         matchable && !m.sequenceMatches(*matchable));
    item(clip, QStringLiteral("Reveal in Media"), [&m] { m.revealInMedia(); }, ShortcutAction::revealInMedia, m.revealableAsset() != nullptr);
    clip->addSeparator();
    item(clip, QStringLiteral("Delete"), [&m] { m.deleteSelected(); }, ShortcutAction::delete_, m.canDeleteSelected());
    item(clip, QStringLiteral("Ripple Delete"), [&m] { m.deleteSelected(true); }, ShortcutAction::rippleDelete, m.canDeleteSelected());
    item(clip, QStringLiteral("Ripple Trim Start to Playhead"), [&m] { m.rippleTrim(true); }, ShortcutAction::rippleTrimStart, m.canRippleTrim());
    item(clip, QStringLiteral("Ripple Trim End to Playhead"), [&m] { m.rippleTrim(false); }, ShortcutAction::rippleTrimEnd, m.canRippleTrim());
    item(clip, QStringLiteral("Close All Gaps"), [&m] { m.closeAllGaps(); });
    clip->addSeparator();
    item(clip, QStringLiteral("Add Text"), [&m] { m.addAtPlayhead(DragPayload::text()); }, ShortcutAction::addText);
    item(clip, QStringLiteral("Add Solid"), [&m] { m.addAtPlayhead(DragPayload::solid()); });
    item(clip, m.captionsMenuTitle(), [&m] { m.captionsFromMenu(); }, std::nullopt, m.canCaption());
    clip->addSeparator();
    item(clip, QStringLiteral("Add Video Track"), [&m] { m.addTrack(TrackKind::video); });
    item(clip, QStringLiteral("Add Audio Track"), [&m] { m.addTrack(TrackKind::audio); });
    // Mark
    QMenu* mark = menuBar()->addMenu(QStringLiteral("Mark"));
    item(mark, QStringLiteral("Add Marker"), [&m] { m.addMarker(); }, ShortcutAction::addMarker);
    item(mark, QStringLiteral("Clear All Markers"), [&m] { m.clearMarkers(); }, std::nullopt, m.project().markers && !m.project().markers->empty());
    mark->addSeparator();
    item(mark, QStringLiteral("Set In"), [&m] { m.setInPoint(); }, ShortcutAction::setIn);
    item(mark, QStringLiteral("Set Out"), [&m] { m.setOutPoint(); }, ShortcutAction::setOut);
    item(mark, QStringLiteral("Clear In and Out"), [&m] { m.clearRange(); }, ShortcutAction::clearInOut, m.project().inPoint || m.project().outPoint);
    // View
    QMenu* view = menuBar()->addMenu(QStringLiteral("View"));
    item(view, QStringLiteral("Zoom In"), [this] { c_->model().zoom(1.4); }, ShortcutAction::zoomIn);
    item(view, QStringLiteral("Zoom Out"), [this] { c_->model().zoom(1 / 1.4); }, ShortcutAction::zoomOut);
    item(view, QStringLiteral("Zoom to Fit"), [this] { c_->model().zoomToFit(); }, ShortcutAction::zoomToFit);
    view->addSeparator();
    auto toggle = [&](const QString& t, bool on, std::function<void()> act, std::optional<ShortcutAction> a) {
        QAction* x = item(view, t, [this, act] { act(); timeline_->refreshToggles(); rebuildMenus(); }, a);
        x->setCheckable(true);
        x->setChecked(on);
    };
    toggle(QStringLiteral("Filmstrips"), m.thumbnails(), [&m] { m.setThumbnails(!m.thumbnails()); }, std::nullopt);
    toggle(QStringLiteral("Snapping"), m.snapping, [&m] { m.snapping = !m.snapping; }, ShortcutAction::snapping);
    toggle(QStringLiteral("Sticky Edges"), m.sticky(), [&m] { m.setSticky(!m.sticky()); }, ShortcutAction::sticky);
    toggle(QStringLiteral("Skimming"), m.skimming(), [&m] { m.setSkimming(!m.skimming()); }, ShortcutAction::skimming);
    toggle(QStringLiteral("Audio Scrubbing"), m.audioScrub(), [&m] { m.setAudioScrub(!m.audioScrub()); }, ShortcutAction::audioScrubbing);
    view->addSeparator();
    QAction* fs = view->addAction(isFullScreen() ? QStringLiteral("Exit Full Screen") : QStringLiteral("Enter Full Screen"), this,
                                  [this] { isFullScreen() ? showNormal() : showFullScreen(); rebuildMenus(); });
    fs->setShortcut(QKeySequence(QStringLiteral("F11")));
    // Window: the Mac puts tab switching in its Window menu.
    QMenu* window = menuBar()->addMenu(QStringLiteral("Window"));
    item(window, QStringLiteral("Show Previous Tab"), [this] { w_.selectPrevious(); }, ShortcutAction::previousTab, w_.count() > 1);
    item(window, QStringLiteral("Show Next Tab"), [this] { w_.selectNext(); }, ShortcutAction::nextTab, w_.count() > 1);
    // Help (the Mac's app menu items: About Editroom, Check for Updates…)
    QMenu* help = menuBar()->addMenu(QStringLiteral("Help"));
    help->addAction(QStringLiteral("About EditroomLX"), this, &MainWindow::showAbout);
    help->addAction(QStringLiteral("Check for Updates…"), this, [this] {
        Updates::shared().checkAndTell(this);
    });
}

void MainWindow::showExport() {
    if (c_->model().project().clips.empty()) {
        Alert::run(this, QStringLiteral("The timeline is empty."), QString(), {QStringLiteral("OK")});
        return;
    }
    ExportDialog d(*c_, this);
    d.exec();
}

void MainWindow::showSettings() {
    SettingsDialog d(*c_, keys_, this);
    connect(&d, &SettingsDialog::themeChanged, this, &MainWindow::applyTheme);
    connect(&d, &SettingsDialog::shortcutsChanged, this, &MainWindow::rebuildMenus);
    connect(&d, &SettingsDialog::magicEnabledChanged, this, [this](bool on) { magicButton_->setVisible(on); });
    d.exec();
}

void MainWindow::showAbout() {
    // The standard About panel: the icon, the name, the version, and one quiet line of credits under it.
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("About EditroomLX"));
    d.setFixedWidth(300);
    auto* v = new QVBoxLayout(&d);
    v->setContentsMargins(20, 24, 20, 20);
    v->setSpacing(6);
    auto* icon = new QLabel;
    icon->setPixmap(QIcon(QStringLiteral(":/editroom.png")).pixmap(64, 64).isNull() ? windowIcon().pixmap(64, 64) : QIcon(QStringLiteral(":/editroom.png")).pixmap(64, 64));
    icon->setAlignment(Qt::AlignCenter);
    v->addWidget(icon);
    v->addSpacing(6);
    auto* name = new QLabel(QStringLiteral("EditroomLX"));
    name->setFont(Theme::font(14, 700));
    name->setAlignment(Qt::AlignCenter);
    v->addWidget(name);
    auto* version = new QLabel(QStringLiteral("Version %1").arg(QCoreApplication::applicationVersion()));
    version->setFont(Theme::font(11));
    version->setAlignment(Qt::AlignCenter);
    v->addWidget(version);
    v->addSpacing(8);
    // Links in the text's own colour, underlined, not the system blue.
    const QString link = QStringLiteral("<a style=\"color:%1;\" href=\"%2\">%3</a>");
    const QString col = Theme::textDim().name();
    auto* credits = new QLabel(QStringLiteral("Uses %1, licensed under the %2<br>%3 (OFL), %4 (MIT-0), %5 (MIT)")
                                   .arg(link.arg(col, QStringLiteral("https://ffmpeg.org"), QStringLiteral("FFmpeg")),
                                        link.arg(col, QStringLiteral("https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html"), QStringLiteral("LGPL 2.1")),
                                        link.arg(col, QStringLiteral("https://rsms.me/inter/"), QStringLiteral("Inter")),
                                        link.arg(col, QStringLiteral("https://miniaud.io"), QStringLiteral("miniaudio")),
                                        link.arg(col, QStringLiteral("https://signalsmith-audio.co.uk/code/stretch/"), QStringLiteral("Signalsmith Stretch"))));
    credits->setFont(Theme::font(11));
    credits->setAlignment(Qt::AlignCenter);
    credits->setWordWrap(true);
    credits->setOpenExternalLinks(true);
    credits->setStyleSheet(QStringLiteral("color: %1;").arg(col));
    v->addWidget(credits);
    d.exec();
}

void MainWindow::perform(ShortcutAction a) {
    EditorModel& m = c_->model();
    using A = ShortcutAction;
    switch (a) {
    case A::playPause: m.togglePlay(); break;
    case A::stepBack: m.step(-1); break;
    case A::stepForward: m.step(1); break;
    case A::previousEdit: m.jumpToEdit(false); break;
    case A::nextEdit: m.jumpToEdit(true); break;
    case A::goToStart: m.seek(0); break;
    case A::goToEnd: m.seek(m.project().duration()); break;
    case A::loop: m.setLoop(!m.loop()); break;
    case A::fullScreenViewer: m.toggleViewerFullScreen(); break;
    case A::selectTool: m.tool = Tool::move; timeline_->refreshToggles(); break;
    case A::cutTool: m.tool = Tool::blade; timeline_->refreshToggles(); break;
    case A::delete_: m.deleteSelected(); break;
    case A::rippleDelete: m.deleteSelected(true); break;
    case A::rippleTrimStart: m.rippleTrim(true); break;
    case A::rippleTrimEnd: m.rippleTrim(false); break;
    case A::revealInMedia: m.revealInMedia(); break;
    case A::addMarker: m.addMarker(); break;
    case A::setIn: m.setInPoint(); break;
    case A::setOut: m.setOutPoint(); break;
    case A::clearInOut: m.clearRange(); break;
    case A::zoomIn: c_->model().zoom(1.4); break;
    case A::zoomOut: c_->model().zoom(1 / 1.4); break;
    case A::zoomToFit: c_->model().zoomToFit(); break;
    case A::snapping: m.snapping = !m.snapping; timeline_->refreshToggles(); break;
    case A::sticky: m.setSticky(!m.sticky()); timeline_->refreshToggles(); break;
    case A::skimming: m.setSkimming(!m.skimming()); timeline_->refreshToggles(); break;
    case A::audioScrubbing: m.setAudioScrub(!m.audioScrub()); timeline_->refreshToggles(); break;
    case A::freezeFrame: m.freezeFrame(); break;
    case A::reverse: m.reverseSelection(); break;
    case A::newTab: w_.newTab(); return;
    case A::closeTab: w_.closeActive(); return;
    case A::previousTab: w_.selectPrevious(); return;
    case A::nextTab: w_.selectNext(); return;
    default: break;
    }
    rebuildMenus();
}

/// KeyCommands: plain keys and arrows are the editor window's, never a menu's; typed as text in a text field.
bool MainWindow::runShortcut(QKeyEvent* e, bool inTextField) {
    auto combo = KeyCombo::fromKey(e->key(), e->text(), int(e->modifiers() & ~Qt::KeypadModifier));
    if (!combo) return false;
    if (inTextField && combo->typesText()) return false;
    auto action = keys_.action(*combo);
    if (!action) return false;
    // Menu items with a modifier shortcut run from the menu itself.
    if (Shortcut::inMenu(*action) && keys_.menu(*action) && !keys_.menu(*action)->typesText() && keys_.menu(*action) == combo) return false;
    perform(*action);
    return true;
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::KeyPress && isActiveWindow()) {
        auto* k = static_cast<QKeyEvent*>(e);
        QWidget* w = qobject_cast<QWidget*>(o);
        if (w && w->window() == this) {
            const bool text = qobject_cast<QLineEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) || w->inherits("QAbstractSpinBox") || w->inherits("QComboBox");
            if (runShortcut(k, text)) return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::setViewerOnly(bool on) {
    for (QWidget* w : std::initializer_list<QWidget*>{library_, inspector_, timeline_, strip_, menuBar()}) w->setVisible(!on);
    if (on) {
        wasFullScreen_ = isFullScreen();
        showFullScreen();
    } else if (!wasFullScreen_) {
        showNormal();
    }
    rebuildMenus();
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    // Esc leaves the full-screen viewer.
    if (e->key() == Qt::Key_Escape && c_->model().viewerOnly()) { c_->model().toggleViewerFullScreen(); return; }
    if (!runShortcut(e, false)) QMainWindow::keyPressEvent(e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // With Auto-save off, each tab's unsaved changes ask Save, Don't Save or Cancel, in front.
    if (!w_.confirmQuit()) { e->ignore(); return; }
    w_.autosaveAll();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasUrls()) return;
    // It lights up the Media panel, where they go, rather than the whole window.
    c_->setImportDropHover(true);
    e->acceptProposedAction();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent*) { c_->setImportDropHover(false); }

void MainWindow::dropEvent(QDropEvent* e) {
    c_->setImportDropHover(false);
    QStringList media;
    for (const QUrl& u : e->mimeData()->urls()) {
        if (!u.isLocalFile()) continue;
        const QString p = u.toLocalFile();
        // A project opens in a tab of its own; anything else is imported here.
        if (EditorModel::isProjectFile(p)) w_.open(p);
        else media << p;
    }
    c_->importFiles(media);
}

} // namespace er
