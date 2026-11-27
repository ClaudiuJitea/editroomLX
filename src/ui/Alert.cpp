#include "Alert.h"
#include "Theme.h"
#include <QApplication>
#include <QHBoxLayout>
#include <cmath>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace er {

Alert::Alert(const QString& message, const QString& info, const QStringList& buttons, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("EditroomLX"));
    setModal(true);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(22, 20, 22, 16);
    v->setSpacing(8);
    auto* title = new QLabel(message);
    title->setFont(Theme::font(13, 700));
    title->setWordWrap(true);
    v->addWidget(title);
    if (!info.isEmpty()) {
        auto* body = new QLabel(info);
        body->setFont(Theme::font(11.5));
        body->setWordWrap(true);
        body->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDim().name()));
        v->addWidget(body);
    }
    v->addSpacing(8);
    auto* row = new QHBoxLayout;
    row->addStretch();
    const QStringList list = buttons.isEmpty() ? QStringList{QStringLiteral("OK")} : buttons;
    // Right to left: the first button is the default, rightmost.
    for (qsizetype i = list.size() - 1; i >= 0; --i) {
        auto* b = new QPushButton(list[i]);
        if (i == 0) { b->setProperty("primary", true); b->setDefault(true); }
        connect(b, &QPushButton::clicked, this, [this, i] { choice_ = int(i); accept(); });
        row->addWidget(b);
    }
    v->addLayout(row);
    setMinimumWidth(380);
    setMaximumWidth(460);
}

int Alert::run(QWidget* parent, const QString& message, const QString& info, const QStringList& buttons) {
    Alert a(message, info, buttons, parent);
    // Centred on Editroom's window across, and a little above its middle (10% of its height), where a box looks
    // centred; placed before it opens, so it doesn't show elsewhere for a frame and then move. (Wayland doesn't let
    // clients place windows: there the compositor centres it on its parent.)
    QWidget* over = parent ? parent->window() : QApplication::activeWindow();
    if (over && over->isVisible()) {
        a.adjustSize();
        const QRect f = over->frameGeometry();
        const QSize size = a.sizeHint().expandedTo(a.minimumSize());
        a.move(f.center().x() - size.width() / 2, int(std::round(f.center().y() - f.height() * 0.1 - size.height() / 2.0)));
    }
    a.exec();
    if (a.choice() >= 0) return a.choice();
    for (qsizetype i = 0; i < buttons.size(); ++i)
        if (buttons[i] == QLatin1String("Cancel") || buttons[i] == QLatin1String("Not Now")) return int(i);
    return int(std::max<qsizetype>(0, buttons.size() - 1));
}

} // namespace er
