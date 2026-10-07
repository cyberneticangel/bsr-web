#include "race.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace bsr {

static V3 bez(const V3& p0, const V3& p1, const V3& p2, const V3& p3, float t) {
    float u = 1 - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

static Path splineToPath(const std::vector<Vec3>& v) {
    Path p;
    size_t nk = v.size() / 3;
    auto K = [&](size_t i, int k) { const Vec3& a = v[i * 3 + k]; return V3(a.x, a.y, a.z); };
    for (size_t i = 0; i + 1 < nk; i++)
        for (int s = 0; s < 8; s++) p.pts.push_back(bez(K(i, 0), K(i, 2), K(i + 1, 1), K(i + 1, 0), s / 8.0f));
    if (nk) p.pts.push_back(K(nk - 1, 0));
    // drop duplicates
    std::vector<V3> clean;
    for (auto& q : p.pts)
        if (clean.empty() || len(q - clean.back()) > 1e-3f) clean.push_back(q);
    if (clean.size() > 2 && len(clean.front() - clean.back()) < 1e-3f) clean.pop_back();
    p.pts = clean;
    p.dist.resize(p.pts.size());
    float d = 0;
    for (size_t i = 0; i < p.pts.size(); i++) {
        p.dist[i] = d;
        d += len(p.pts[(i + 1) % p.pts.size()] - p.pts[i]);
    }
    p.length = d;
    return p;
}

int Path::nearest(const V3& q, int hint, int window) const {
    int n = (int)pts.size();
    if (!n) return 0;
    int best = 0;
    float bd = 1e30f;
    if (window <= 0 || window >= n) {
        for (int i = 0; i < n; i++) {
            V3 d = pts[i] - q;
            d.z *= 0.3f;
            float dd = dot(d, d);
            if (dd < bd) { bd = dd; best = i; }
        }
        return best;
    }
    for (int k = -window; k <= window; k++) {
        int i = ((hint + k) % n + n) % n;
        V3 d = pts[i] - q;
        d.z *= 0.3f;
        float dd = dot(d, d);
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}

V3 Path::at(float d) const {
    if (pts.empty()) return V3();
    d = std::fmod(d, length);
    if (d < 0) d += length;
    size_t i = std::upper_bound(dist.begin(), dist.end(), d) - dist.begin();
    if (i == 0) i = 1;
    size_t a = i - 1, b = i % pts.size();
    float seg = (b == 0 ? length : dist[b]) - dist[a];
    float t = seg > 1e-6f ? (d - dist[a]) / seg : 0;
    return pts[a] + (pts[b] - pts[a]) * t;
}

V3 Path::dirAt(float d) const { return norm(at(d + 0.5f) - at(d - 0.5f)); }

bool TrackMeta::load(const std::string& track) {
    FSO f;
    if (!loadFSO("tracks/" + track + "_meta.fso", f)) return false;
    std::map<std::string, std::vector<Vec3>> objs;
    for (auto& c : f.root.children) {
        if (c.geoset < 0 || c.geoset >= (int)f.geosets.size() || f.geosets[c.geoset].lods.empty()) continue;
        objs[lower(c.name)] = f.geosets[c.geoset].lods[0].pos;
    }
    for (auto& [name, v] : objs) {  // std::map iterates sorted by name
        if (v.size() < 6) continue;
        V3 k0(v[0].x, v[0].y, v[0].z), k1(v[3].x, v[3].y, v[3].z);
        if (name.find("gridpos") != std::string::npos) {
            grid.push_back({k0, std::atan2(k1.y - k0.y, k1.x - k0.x)});
        } else if (name.find("checkpoint") != std::string::npos) {
            if (name.back() == 'p') continue;  // pit-lane checkpoint
            checkpoints.push_back({k0, k1});
        } else if (name.find("aii") != std::string::npos) {
            aiLines.push_back(splineToPath(v));
        } else if (name.find("aisl") != std::string::npos) {
            left = splineToPath(v);
        } else if (name.find("aisr") != std::string::npos) {
            right = splineToPath(v);
        }
    }
    return !grid.empty() && !checkpoints.empty() && !aiLines.empty();
}

bool crossed(const V3& p0, const V3& p1, const Gate& g) {
    auto cr = [](const V3& o, const V3& a, const V3& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    return cr(p0, p1, g.a) * cr(p0, p1, g.b) < 0 && cr(g.a, g.b, p0) * cr(g.a, g.b, p1) < 0;
}

void driveAI(Racer& r, const TrackMeta& meta, float dt) {
    Car& c = r.car;
    const Path& line = meta.aiLines[r.aiLine % meta.aiLines.size()];
    r.pathIdx = line.nearest(c.pos, r.pathIdx, 40);
    float speed = c.speed();
    float d0 = line.dist[r.pathIdx];
    float look = 0.7f + std::max(speed, 0.0f) * 0.28f;
    V3 target = line.at(d0 + look);
    V3 to = target - c.pos;
    float lx = dot(to, c.R.col(0)), ly = dot(to, c.R.col(1));
    float ang = std::atan2(lx, ly);
    c.steer = clampf(ang / c.spec.steerMax * 1.6f, -1, 1);

    // Speed limit: cornering speed sqrt(a_lat * R) at each point ahead, reachable with the braking we have.
    const float aLat = c.spec.grip * 9.81f * 0.8f, aBrake = 5.0f;
    float vmax = c.spec.topSpeed * r.aiSkill;
    float targetV = vmax;
    float horizon = 2.0f + speed * speed / (2 * aBrake) + 1.0f;
    for (float s = 0.0f; s <= horizon; s += 0.5f) {
        V3 a = line.dirAt(d0 + s - 0.6f), b = line.dirAt(d0 + s + 0.6f);
        float k = std::acos(clampf(dot(a, b), -1, 1)) / 1.2f;  // curvature 1/R
        float vc = std::sqrt(aLat / std::max(k, 1e-3f)) * (0.85f + 0.15f * r.aiSkill);
        targetV = std::min(targetV, std::sqrt(vc * vc + 2 * aBrake * s));
    }
    targetV = std::max(targetV, 3.0f);
    if (speed < targetV) { c.throttle = clampf((targetV - speed) * 0.8f + 0.3f, 0, 1); c.brake = 0; }
    else { c.throttle = 0; c.brake = clampf((speed - targetV) * 1.5f + 0.2f, 0, 1); }
    if (c.hold) { r.stuckTime = 0; return; }

    // Stuck: back up with opposite lock.
    if (r.reverseTime > 0) {
        r.reverseTime -= dt;
        c.throttle = 0;
        c.brake = 1;
        c.steer = -c.steer;
        return;
    }
    if (std::fabs(speed) < 0.4f && c.throttle > 0.3f) r.stuckTime += dt;
    else r.stuckTime = std::max(0.0f, r.stuckTime - dt);
    if (r.stuckTime > 1.0f) {
        r.stuckTime = 0;
        r.reverseTime = 1.0f;
        // Repeatedly stuck: ask the game to put us back on the racing line.
        r.stuckCount = (r.stuckClock > 0) ? r.stuckCount + 1 : 1;
        r.stuckClock = 8.0f;
        if (r.stuckCount >= 3) { r.needsReset = true; r.stuckCount = 0; r.reverseTime = 0; }
    }
    r.stuckClock = std::max(0.0f, r.stuckClock - dt);
}

}  // namespace bsr
