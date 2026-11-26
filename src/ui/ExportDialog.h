// ExportSheet.swift: Filename, Location, Range, Format, Loudness, Resolution, Shape, Codec, Quality,
// Audio; Cancel and Export. The export joins the queue and runs in the background.
#pragma once
#include "Controller.h"
#include <QDialog>

namespace er {

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(Controller& c, QWidget* parent = nullptr);
    static const std::vector<std::pair<QString, std::optional<double>>>& loudnessTargets();
    static QString loudnessLabel(std::optional<double> v);
    static QString rangeLength(double s);
private:
    void build();
    void accept() override;
    Controller& c_;
    ExportSettings settings_;
    QString name_, folder_;
};

} // namespace er
