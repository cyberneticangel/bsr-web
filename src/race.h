#pragma once
#include <string>
#include <vector>

#include "formats.h"
#include "mathx.h"
#include "physics.h"

namespace bsr {

// A closed polyline sampled from a meta Bezier spline (knots stored as [knot, in, out] triples).
struct Path {
    std::vector<V3> pts;
    std::vector<float> dist;  // cumulative
    float length = 0;
    int nearest(const V3& p, int hint, int window) const;
    V3 at(float d) const;
    V3 dirAt(float d) const;
};

struct Gate { V3 a, b; };
struct GridSlot { V3 pos; float heading; };

struct TrackMeta {
    std::vector<GridSlot> grid;
    std::vector<Gate> checkpoints;  // racing checkpoints, [0] is start/finish
    std::vector<Path> aiLines;
    Path left, right;
    bool load(const std::string& track);
};

struct Racer {
    Car car;
    bool ai = true;
    int aiLine = 0;
    float aiSkill = 1.0f;
    int pathIdx = 0;
    float progress = 0;     // along ai line 0
    int nextCp = 0;
    int lap = 0;            // laps started (crossing start line increments)
    bool finished = false;
    float finishTime = 0;
    float lapStart = 0, bestLap = 0, lastLap = 0;
    float stuckTime = 0, reverseTime = 0, flipTime = 0, stuckClock = 0;
    int stuckCount = 0;
    bool needsReset = false;
    int skin = 0;
    V3 prevPos;
    int place = 0;
};

void driveAI(Racer& r, const TrackMeta& meta, float dt);
// Returns true when the car crossed the gate a->b between p0 and p1 (2D, either direction).
bool crossed(const V3& p0, const V3& p1, const Gate& g);

}  // namespace bsr
