#include "netsync.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bsr {

static const double kInterpDelay = 0.1;   // seconds behind the newest data; covers ~2 lost/late packets at 20 Hz
static const float kMaxExtrapolate = 0.25f;
static const size_t kMaxSnaps = 32;

static void put(float* o, const V3& v) { o[0] = v.x; o[1] = v.y; o[2] = v.z; }
static V3 get(const float* o) { return V3(o[0], o[1], o[2]); }
static V3 mix(const float* a, const float* b, float t) { return get(a) + (get(b) - get(a)) * t; }

void packSnapshot(const Racer& r, float clock, float* o) {
    const Car& c = r.car;
    memset(o, 0, sizeof(float) * kSnapFloats);
    o[kSnapTime] = clock;
    put(o + kSnapPos, c.pos);
    Q q = Q::fromM3(c.R);
    o[kSnapQuat] = q.x; o[kSnapQuat + 1] = q.y; o[kSnapQuat + 2] = q.z; o[kSnapQuat + 3] = q.w;
    put(o + kSnapVel, c.vel);
    put(o + kSnapAngVel, c.angVel);
    for (size_t i = 0; i < c.wheels.size() && i < 4; i++) {
        if (c.wheels[i].front) o[kSnapSteer] = c.wheels[i].steer;
        o[kSnapComp + i] = c.wheels[i].comp;
        o[kSnapSpin + i] = c.wheels[i].spin;
    }
    o[kSnapLap] = (float)r.lap;
    o[kSnapNextCp] = (float)r.nextCp;
    o[kSnapFinished] = r.finished ? 1.0f : 0.0f;
    o[kSnapFinishTime] = r.finishTime;
    o[kSnapBestLap] = r.bestLap;
    o[kSnapLastLap] = r.lastLap;
    o[kSnapRpm] = c.rpm;
    o[kSnapSkid] = c.skid;
}

void SnapshotBuffer::push(const float* d, double now) {
    for (int i = 0; i < kSnapFloats; i++)
        if (!std::isfinite(d[i])) return;
    if (!snaps_.empty() && d[kSnapTime] <= snaps_.back().d[kSnapTime]) return;  // stale or duplicate
    double sample = now - d[kSnapTime];
    if (!haveOffset_ || sample < offset_) offset_ = sample;
    else offset_ += (sample - offset_) * 0.002;  // follow slowly if the route got slower for good
    haveOffset_ = true;
    Snap s;
    memcpy(s.d, d, sizeof s.d);
    snaps_.push_back(s);
    while (snaps_.size() > kMaxSnaps) snaps_.pop_front();
}

bool SnapshotBuffer::apply(Racer& r, double now) const {
    if (snaps_.empty()) return false;
    double rt = now - offset_ - kInterpDelay;  // render time on the sender's clock
    size_t bi = 0;
    while (bi < snaps_.size() && snaps_[bi].d[kSnapTime] < rt) bi++;
    const float* a;
    const float* b;
    float t = 0, extra = 0;
    if (bi == snaps_.size()) {  // newest data is older than we want: extrapolate briefly
        a = b = snaps_.back().d;
        extra = (float)std::min<double>(rt - a[kSnapTime], kMaxExtrapolate);
    } else if (bi == 0) {
        a = b = snaps_.front().d;
    } else {
        a = snaps_[bi - 1].d;
        b = snaps_[bi].d;
        float span = b[kSnapTime] - a[kSnapTime];
        t = span > 1e-6f ? clampf((float)(rt - a[kSnapTime]) / span, 0, 1) : 1;
    }
    Car& c = r.car;
    Q qa{a[kSnapQuat], a[kSnapQuat + 1], a[kSnapQuat + 2], a[kSnapQuat + 3]};
    Q qb{b[kSnapQuat], b[kSnapQuat + 1], b[kSnapQuat + 2], b[kSnapQuat + 3]};
    c.pos = mix(a + kSnapPos, b + kSnapPos, t);
    c.R = Q::nlerp(qa, qb, t).toM3();
    c.vel = mix(a + kSnapVel, b + kSnapVel, t);
    c.angVel = mix(a + kSnapAngVel, b + kSnapAngVel, t);
    if (extra > 0) {
        c.pos += c.vel * extra;
        c.R.rotate(c.angVel, extra);
    }
    for (size_t i = 0; i < c.wheels.size() && i < 4; i++) {
        Wheel& w = c.wheels[i];
        w.steer = w.front ? lerpf(a[kSnapSteer], b[kSnapSteer], t) : 0;
        w.comp = lerpf(a[kSnapComp + i], b[kSnapComp + i], t);
        w.spin = lerpf(a[kSnapSpin + i], b[kSnapSpin + i], t);
    }
    c.rpm = lerpf(a[kSnapRpm], b[kSnapRpm], t);
    c.skid = lerpf(a[kSnapSkid], b[kSnapSkid], t);
    c.impact = 0;
    // Race progress as reported by the owner (it sees every checkpoint crossing; we only see samples).
    r.lap = (int)a[kSnapLap];
    r.nextCp = (int)a[kSnapNextCp];
    r.finished = a[kSnapFinished] != 0;
    r.finishTime = a[kSnapFinishTime];
    r.bestLap = a[kSnapBestLap];
    r.lastLap = a[kSnapLastLap];
    return true;
}

}  // namespace bsr
