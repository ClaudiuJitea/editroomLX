#include "Loudness.h"
#include <algorithm>
#include <cmath>

namespace er::LoudnessMeter {

namespace {
struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
// K-weighting at 48 kHz: a high shelf, then a high-pass.
Biquad shelf() { return {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585}; }
Biquad highPass() { return {1, -2, 1, -1.99004745483398, 0.99007225036621}; }
double lufs(double power) { return -0.691 + 10 * std::log10(std::max(power, 1e-12)); }
} // namespace

std::optional<Result> measure(const std::vector<float>& data) {
    Biquad l0 = shelf(), l1 = highPass(), r0 = shelf(), r1 = highPass();
    // Mean square in 100ms steps; blocks are four steps (400ms, 75% overlap).
    const int step = 4800;
    std::vector<double> steps;
    double acc = 0;
    int count = 0;
    float peak = 0;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        const float l = data[i], r = data[i + 1];
        peak = std::max({peak, std::abs(l), std::abs(r)});
        const double x = l1.run(l0.run(double(l)));
        const double y = r1.run(r0.run(double(r)));
        acc += x * x + y * y;
        if (++count == step) {
            steps.push_back(acc / step);
            acc = 0;
            count = 0;
        }
    }
    if (steps.size() < 4) return std::nullopt;
    std::vector<double> blocks;
    for (size_t k = 0; k + 4 <= steps.size(); ++k) blocks.push_back((steps[k] + steps[k + 1] + steps[k + 2] + steps[k + 3]) / 4);
    std::vector<double> absolute;
    for (double b : blocks) if (lufs(b) > -70) absolute.push_back(b);
    if (absolute.empty()) return std::nullopt;
    double sum = 0;
    for (double b : absolute) sum += b;
    const double relativeGate = lufs(sum / absolute.size()) - 10;
    double gsum = 0;
    int gn = 0;
    for (double b : absolute) if (lufs(b) > relativeGate) { gsum += b; ++gn; }
    if (gn == 0) return std::nullopt;
    return Result{lufs(gsum / gn), peak > 0 ? 20 * std::log10(double(peak)) : -120};
}

double targetGain(const Result& r, double target) { return std::min(target - r.lufs, -1 - r.peakDB); }

} // namespace er::LoudnessMeter
