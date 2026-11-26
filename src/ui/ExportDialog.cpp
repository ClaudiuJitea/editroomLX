#include "ExportDialog.h"
#include "Theme.h"
#include "Widgets.h"
#include "model/Timecode.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <cmath>

namespace er {

const std::vector<std::pair<QString, std::optional<double>>>& ExportDialog::loudnessTargets() {
    static const std::vector<std::pair<QString, std::optional<double>>> t = {
        {"As mixed", std::nullopt}, {"−14 LUFS · web, YouTube", -14.0}, {"−16 LUFS · podcasts", -16.0}, {"−23 LUFS · broadcast", -23.0}};
    return t;
}

QString ExportDialog::loudnessLabel(std::optional<double> v) {
    if (!v) return QStringLiteral("As mixed");
    return QStringLiteral("%1 LUFS").arg(int(*v)).replace(u'-', QStringLiteral("−"));
}

QString ExportDialog::rangeLength(double s) {
    if (s < 60) return s < 10 ? QString::asprintf("%.1fs", s) : QStringLiteral("%1s").arg(int(std::round(s)));
    return Timecode::shortString(s);
}

ExportDialog::ExportDialog(Controller& c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(QStringLiteral("Export"));
    const Project& p = c_.model().project();
    name_ = p.exportName.value_or(c_.model().title());
    folder_ = p.exportDir.value_or(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    build();
}

void ExportDialog::build() {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 22);
    auto* title = new QLabel(QStringLiteral("Export"));
    title->setFont(Theme::font(16, 600));
    v->addWidget(title);
    v->addSpacing(10);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    const Project& p = c_.model().project();
    auto* name = new QLineEdit(name_);
    auto* ext = new QLabel(QStringLiteral(".") + settings_.fileExtension());
    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(name, 1);
    nameRow->addWidget(ext);
    form->addRow(QStringLiteral("Filename"), nameRow);
    connect(name, &QLineEdit::textChanged, this, [this](const QString& t) { name_ = t; });
    auto* folder = new QPushButton(QFileInfo(folder_).fileName().isEmpty() ? folder_ : QFileInfo(folder_).fileName());
    connect(folder, &QPushButton::clicked, this, [this, folder] {
        const QString d = QFileDialog::getExistingDirectory(this, QString(), folder_);
        if (d.isEmpty()) return;
        folder_ = d;
        folder->setText(QFileInfo(d).fileName());
    });
    form->addRow(QStringLiteral("Location"), folder);
    if (auto r = p.playRange()) {
        auto* range = new QComboBox;
        range->addItem(QStringLiteral("In to out · ") + rangeLength(r->end - r->start));
        range->addItem(QStringLiteral("Whole timeline"));
        range->setCurrentIndex(1);
        connect(range, &QComboBox::activated, this, [this](int i) { settings_.rangeOnly = i == 0; });
        form->addRow(QStringLiteral("Range"), range);
    }
    auto* format = new QComboBox;
    format->addItems({QStringLiteral("MP4 Video"), QStringLiteral("WAV Audio")});
    form->addRow(QStringLiteral("Format"), format);
    auto* res = new QComboBox;
    res->addItem(QStringLiteral("Sequence  %1 × %2").arg(p.width).arg(p.height));
    const std::vector<std::pair<QString, int>> base = {{"4K", 2160}, {"1440p", 1440}, {"1080p", 1080}, {"720p", 720}};
    for (const auto& [l, h] : base) res->addItem(l, h);
    connect(res, &QComboBox::activated, this, [this, res](int i) { settings_.height = i == 0 ? std::nullopt : std::optional<int>(res->itemData(i).toInt()); });
    form->addRow(QStringLiteral("Resolution"), res);
    auto* shape = new QComboBox;
    for (auto s : {ExportSettings::Shape::original, ExportSettings::Shape::vertical, ExportSettings::Shape::square, ExportSettings::Shape::portrait})
        shape->addItem(ExportSettings::label(s));
    connect(shape, &QComboBox::activated, this, [this](int i) { settings_.shape = ExportSettings::Shape(i); settings_.reframeX = settings_.reframeY = 0.5; });
    form->addRow(QStringLiteral("Shape"), shape);
    auto* codec = new QComboBox;
    codec->addItems({QStringLiteral("H.264"), QStringLiteral("HEVC")});
    connect(codec, &QComboBox::activated, this, [this](int i) { settings_.codec = ExportSettings::Codec(i); });
    form->addRow(QStringLiteral("Codec"), codec);
    auto* quality = new QComboBox;
    quality->addItems({QStringLiteral("High"), QStringLiteral("Web")});
    connect(quality, &QComboBox::activated, this, [this](int i) { settings_.quality = ExportSettings::Quality(i); });
    form->addRow(QStringLiteral("Quality"), quality);
    // Whether there's sound, and how loud: one menu ("No audio" only for video; a WAV is its sound).
    auto* audio = new QComboBox;
    auto fillAudio = [this, audio] {
        audio->clear();
        const bool video = settings_.format == ExportSettings::Format::mp4;
        if (video) {
            audio->addItem(QStringLiteral("No audio"));
            audio->insertSeparator(1);
        }
        for (const auto& [label, value] : loudnessTargets()) audio->addItem(label);
        int at = video ? 2 : 0;
        for (size_t k = 0; k < loudnessTargets().size(); ++k) if (loudnessTargets()[k].second == settings_.loudness) at = int(k) + (video ? 2 : 0);
        audio->setCurrentIndex(video && !settings_.includeAudio ? 0 : at);
    };
    fillAudio();
    connect(audio, &QComboBox::activated, this, [this, audio](int i) {
        const bool video = settings_.format == ExportSettings::Format::mp4;
        if (video && i == 0) { settings_.includeAudio = false; return; }
        const int k = i - (video ? 2 : 0);
        if (k < 0 || k >= int(loudnessTargets().size())) return;
        settings_.includeAudio = true;
        settings_.loudness = loudnessTargets()[size_t(k)].second;
        (void)audio;
    });
    form->addRow(QStringLiteral("Audio"), audio);
    // Exported bigger than the sequence: said so, since it's upscaled (sharpened as it's enlarged), not stretched.
    auto* upscaled = new QLabel;
    upscaled->setObjectName(QStringLiteral("upscaledNote"));
    upscaled->setFont(Theme::font(11, 500));
    upscaled->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textDim().name()));
    form->addRow(QString(), upscaled);
    auto refreshUpscaled = [this, upscaled] {
        const Project& p = c_.model().project();
        const auto [w, h] = settings_.renderSize(p);
        const bool up = settings_.format == ExportSettings::Format::mp4 && std::max(w, h) > std::max(p.width, p.height) + 1;
        upscaled->setText(up ? QStringLiteral("Upscaled from %1p").arg(std::min(p.width, p.height)) : QString());
        upscaled->setVisible(up);
    };
    refreshUpscaled();
    connect(res, &QComboBox::activated, this, refreshUpscaled);
    connect(shape, &QComboBox::activated, this, refreshUpscaled);
    connect(format, &QComboBox::activated, this, [this, ext, res, shape, codec, quality, fillAudio, refreshUpscaled](int i) {
        settings_.format = ExportSettings::Format(i);
        const bool video = i == 0;
        ext->setText(QStringLiteral(".") + settings_.fileExtension());
        for (QWidget* w : {static_cast<QWidget*>(res), static_cast<QWidget*>(shape), static_cast<QWidget*>(codec), static_cast<QWidget*>(quality)}) w->setEnabled(video);
        fillAudio();
        refreshUpscaled();
    });
    v->addLayout(form);
    v->addSpacing(16);
    auto* footer = new QHBoxLayout;
    footer->addStretch();
    auto* cancel = new PillButton(QStringLiteral("Cancel"));
    auto* go = new PillButton(QStringLiteral("Export"), true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(go, &QPushButton::clicked, this, &ExportDialog::accept);
    footer->addWidget(cancel);
    footer->addWidget(go);
    v->addLayout(footer);
    setMinimumWidth(420);
}

void ExportDialog::accept() {
    QString base = name_.trimmed();
    if (base.isEmpty()) base = c_.model().title();
    c_.model().setExportTarget(base, folder_);
    const QString path = folder_ + u'/' + base + u'.' + settings_.fileExtension();
    c_.exports().add(c_.model().project(), settings_, path);
    QDialog::accept();
}

} // namespace er
