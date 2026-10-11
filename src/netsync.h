// Network snapshots of cars simulated on another machine: packing, buffering and interpolation.
#pragma once
#include <deque>

#include "race.h"

namespace bsr {

// Wire layout of one car state, in floats.
enum : int {
    kSnapTime = 0,
    kSnapPos = 1,      // 3
    kSnapQuat = 4,     // 4
    kSnapVel = 8,      // 3
    kSnapAngVel = 11,  // 3
    kSnapSteer = 14,
    kSnapComp = 15,    // 4 wheels
    kSnapSpin = 19,    // 4 wheels
    kSnapLap = 23,
    kSnapNextCp,
    kSnapFinished,
    kSnapFinishTime,
    kSnapBestLap,
    kSnapLastLap,
    kSnapRpm,
    kSnapSkid,
    kSnapFloats
};

// clock: sender time in seconds (any origin; the receiver estimates the offset).
void packSnapshot(const Racer& r, float clock, float* out);

// Buffers the snapshots of one remote car and replays them slightly in the past, interpolated, so that
// network jitter doesn't show as stutter.
class SnapshotBuffer {
public:
    void push(const float* snap, double now);
    // Writes the state at `now` into r. Returns false until the first snapshot has arrived.
    bool apply(Racer& r, double now) const;
    void clear() { snaps_.clear(); haveOffset_ = false; }

private:
    struct Snap { float d[kSnapFloats]; };
    std::deque<Snap> snaps_;
    double offset_ = 0;  // local clock - sender clock, tracking the fastest delivery seen
    bool haveOffset_ = false;
};

}  // namespace bsr
