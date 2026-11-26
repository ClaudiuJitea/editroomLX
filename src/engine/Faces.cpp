#include "Faces.h"
#include "Onnx.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace er::Faces {

QString modelPath() {
    if (qEnvironmentVariableIsSet("EDITROOM_FACE_MODEL")) return qEnvironmentVariable("EDITROOM_FACE_MODEL");
    for (const QString& p : {QCoreApplication::applicationDirPath() + QStringLiteral("/../share/editroom/models/face_detection_yunet_2023mar.onnx"),
                             QStringLiteral(ER_SOURCE_DIR "/third_party/models/yunet/face_detection_yunet_2023mar.onnx")})
        if (QFileInfo::exists(p)) return p;
    return {};
}

bool available() { return !modelPath().isEmpty() && onnx::load(); }

namespace {
constexpr int side = 640;

Ort::Session* session() {
    static std::once_flag once;
    static std::unique_ptr<Ort::Session> s;
    std::call_once(once, [] {
        if (!available()) return;
        try {
            Ort::SessionOptions o;
            o.SetIntraOpNumThreads(std::max(1, std::min(4, QThread::idealThreadCount() / 2)));
            s = std::make_unique<Ort::Session>(onnx::env(), modelPath().toLocal8Bit().constData(), o);
        } catch (const Ort::Exception&) { s.reset(); }
    });
    return s.get();
}

double iou(const QRectF& a, const QRectF& b) {
    const QRectF i = a.intersected(b);
    const double ai = i.width() * i.height();
    return ai <= 0 ? 0 : ai / (a.width() * a.height() + b.width() * b.height() - ai);
}
} // namespace

std::vector<QRectF> find(const QImage& image) {
    Ort::Session* s = session();
    if (!s || image.isNull()) return {};
    static std::mutex m;
    std::lock_guard g(m);
    // Fitted into 640 × 640 (the rest black), as BGR 0…255: YuNet's input.
    const double k = double(side) / std::max(image.width(), image.height());
    const QImage fitted = image.convertToFormat(QImage::Format_RGB888)
                              .scaled(std::max(1, int(std::round(image.width() * k))), std::max(1, int(std::round(image.height() * k))),
                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> px(size_t(3) * side * side, 0.f);
    for (int y = 0; y < fitted.height(); ++y) {
        const uchar* row = fitted.constScanLine(y);
        for (int x = 0; x < fitted.width(); ++x)
            for (int c = 0; c < 3; ++c) px[size_t(c) * side * side + size_t(y) * side + x] = row[x * 3 + (2 - c)];
    }
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 4> shape{1, 3, side, side};
    auto input = Ort::Value::CreateTensor<float>(mem, px.data(), px.size(), shape.data(), 4);
    static const char* inName = "input";
    static const char* outNames[] = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "bbox_8", "bbox_16", "bbox_32"};
    std::vector<Ort::Value> out;
    try {
        out = s->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, outNames, 9);
    } catch (const Ort::Exception&) {
        return {};
    }
    struct Hit { QRectF box; double score; };
    std::vector<Hit> hits;
    const int strides[3] = {8, 16, 32};
    for (int i = 0; i < 3; ++i) {
        const int st = strides[i], cols = side / st, n = cols * cols;
        const float* cls = out[i].GetTensorData<float>();
        const float* obj = out[3 + i].GetTensorData<float>();
        const float* box = out[6 + i].GetTensorData<float>();
        for (int j = 0; j < n; ++j) {
            const double score = std::sqrt(std::clamp(double(cls[j]), 0.0, 1.0) * std::clamp(double(obj[j]), 0.0, 1.0));
            if (score < 0.8) continue;
            const int r = j / cols, c = j % cols;
            const double cx = (c + box[j * 4]) * st, cy = (r + box[j * 4 + 1]) * st;
            const double w = std::exp(box[j * 4 + 2]) * st, h = std::exp(box[j * 4 + 3]) * st;
            hits.push_back({QRectF(cx - w / 2, cy - h / 2, w, h), score});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::vector<QRectF> kept;
    for (const auto& h : hits)
        if (std::none_of(kept.begin(), kept.end(), [&](const QRectF& k2) { return iou(k2, h.box) > 0.3; })) kept.push_back(h.box);
    // As fractions of the picture.
    const double fw = fitted.width(), fh = fitted.height();
    std::vector<QRectF> faces;
    for (const auto& b : kept) {
        const QRectF f = QRectF(b.x() / fw, b.y() / fh, b.width() / fw, b.height() / fh).intersected(QRectF(0, 0, 1, 1));
        if (!f.isEmpty()) faces.push_back(f);
    }
    return faces;
}

int count(const QImage& image) {
    int n = 0;
    for (const auto& f : find(image)) if (f.width() * f.height() > 0.004) ++n;
    return n;
}

} // namespace er::Faces
