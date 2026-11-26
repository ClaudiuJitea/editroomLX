// Faces (MediaAnalyzer.swift's VNDetectFaceRectanglesRequest): how many faces a frame shows, tiny ones in the
// background left out. The Mac asks Vision; here YuNet (OpenCV Zoo, MIT, 230 KB, installed with Editroom) runs
// through ONNX Runtime on this computer.
#pragma once
#include <QImage>
#include <QRectF>
#include <QString>
#include <vector>

namespace er::Faces {
/// The detector is here (ONNX Runtime and the model).
bool available();
QString modelPath();
/// Faces in the picture, as fractions of it (x, y from the top left), surest first.
std::vector<QRectF> find(const QImage& image);
/// Faces bigger than 0.4 % of the picture, as the Mac counts them.
int count(const QImage& image);
} // namespace er::Faces
