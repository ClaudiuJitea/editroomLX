// MagicPlan.swift: a finished edit plan (the project to start from and the edits that build the cut, each after
// the ones it waits for), the Choreographer that turns an EditDecision into one, and SeededRandom. Ported
// literally, so the same media always gives the same cut as on the Mac.
#pragma once
#include "MagicEdit.h"
#include "analysis/AutoPlanner.h"

namespace er {

struct MagicTask {
    MagicEdit edit;
    std::vector<int> after;
    /// Extra wait after its dependencies are done, so a group of cursors arrives with a slight stagger.
    double delay = 0;
};

struct MagicPlan {
    Project base;
    std::vector<MagicTask> tasks;
    /// The project once every edit is made.
    Project result() const;
};

/// Repeatable randomness, so the same media always gives the same cut.
struct SeededRandom {
    explicit SeededRandom(uint64_t seed) : state(seed + 0x9E3779B97F4A7C15ULL) {}
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(state >> 11) / double(uint64_t(1) << 53);
    }
    uint64_t state;
};

namespace Choreographer {
std::optional<MagicPlan> plan(const Project& project, const EditDecision& decision);
} // namespace Choreographer

} // namespace er
