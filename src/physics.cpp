#include "physics.h"

#include <algorithm>
#include <cmath>

namespace bsr {

static const float kGravity = 9.81f;

// ---------------------------------------------------------------- collision world

void CollisionWorld::build(const FST& fst) {
    tris_.clear();
    bmin = V3(1e9f, 1e9f, 1e9f);
    bmax = V3(-1e9f, -1e9f, -1e9f);
    for (const CollTri& ct : fst.tris) {
        if (ct.v[0] >= fst.verts.size() || ct.v[1] >= fst.verts.size() || ct.v[2] >= fst.verts.size()) continue;
        Tri t;
        auto cv = [&](uint32_t i) { const Vec3& v = fst.verts[i]; return V3(v.x, v.y, v.z); };
        t.a = cv(ct.v[0]); t.b = cv(ct.v[1]); t.c = cv(ct.v[2]);
        V3 n = cross(t.b - t.a, t.c - t.a);
        if (len(n) < 1e-8f) continue;  // degenerate
        t.n = norm(n);
        t.surface = ct.surface;
        t.lo = V3(std::min({t.a.x, t.b.x, t.c.x}), std::min({t.a.y, t.b.y, t.c.y}), std::min({t.a.z, t.b.z, t.c.z}));
        t.hi = V3(std::max({t.a.x, t.b.x, t.c.x}), std::max({t.a.y, t.b.y, t.c.y}), std::max({t.a.z, t.b.z, t.c.z}));
        bmin = V3(std::min(bmin.x, t.lo.x), std::min(bmin.y, t.lo.y), std::min(bmin.z, t.lo.z));
        bmax = V3(std::max(bmax.x, t.hi.x), std::max(bmax.y, t.hi.y), std::max(bmax.z, t.hi.z));
        tris_.push_back(t);
    }
    nx_ = std::max(1, (int)std::ceil((bmax.x - bmin.x) / cell_) + 1);
    ny_ = std::max(1, (int)std::ceil((bmax.y - bmin.y) / cell_) + 1);
    cells_.assign((size_t)nx_ * ny_, {});
    for (uint32_t i = 0; i < tris_.size(); i++) {
        const Tri& t = tris_[i];
        int x0 = (int)((t.lo.x - bmin.x) / cell_), x1 = (int)((t.hi.x - bmin.x) / cell_);
        int y0 = (int)((t.lo.y - bmin.y) / cell_), y1 = (int)((t.hi.y - bmin.y) / cell_);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) cells_[(size_t)y * nx_ + x].push_back(i);
    }
    stamp_.assign(tris_.size(), 0);
}

template <typename F>
void CollisionWorld::query(float x0, float y0, float x1, float y1, F&& f) const {
    if (tris_.empty()) return;
    frame_++;
    int cx0 = std::max(0, (int)((x0 - bmin.x) / cell_)), cx1 = std::min(nx_ - 1, (int)((x1 - bmin.x) / cell_));
    int cy0 = std::max(0, (int)((y0 - bmin.y) / cell_)), cy1 = std::min(ny_ - 1, (int)((y1 - bmin.y) / cell_));
    for (int y = cy0; y <= cy1; y++)
        for (int x = cx0; x <= cx1; x++)
            for (uint32_t i : cells_[(size_t)y * nx_ + x]) {
                if (stamp_[i] == frame_) continue;
                stamp_[i] = frame_;
                f(tris_[i]);
            }
}

bool CollisionWorld::raycast(const V3& o, const V3& d, float maxT, Hit& hit) const {
    V3 e = o + d * maxT;
    bool found = false;
    hit.t = maxT;
    query(std::min(o.x, e.x), std::min(o.y, e.y), std::max(o.x, e.x), std::max(o.y, e.y), [&](const Tri& t) {
        float dn = dot(d, t.n);
        if (dn >= -1e-6f) return;  // one-sided
        V3 e1 = t.b - t.a, e2 = t.c - t.a;
        V3 p = cross(d, e2);
        float det = dot(e1, p);
        if (std::fabs(det) < 1e-10f) return;
        float inv = 1.0f / det;
        V3 s = o - t.a;
        float u = dot(s, p) * inv;
        if (u < 0 || u > 1) return;
        V3 q = cross(s, e1);
        float v = dot(d, q) * inv;
        if (v < 0 || u + v > 1) return;
        float tt = dot(e2, q) * inv;
        if (tt < 0 || tt >= hit.t) return;
        hit.t = tt;
        hit.n = t.n;
        hit.surface = t.surface;
        found = true;
    });
    return found;
}

static V3 closestOnTri(const V3& p, const V3& a, const V3& b, const V3& c) {
    V3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    V3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    V3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

int CollisionWorld::sphere(const V3& c, float r, Contact* out, int maxOut) const {
    int n = 0;
    query(c.x - r, c.y - r, c.x + r, c.y + r, [&](const Tri& t) {
        if (n >= maxOut) return;
        if (c.z + r < t.lo.z || c.z - r > t.hi.z) return;
        float sd = dot(c - t.a, t.n);
        if (sd > r || sd < -r) return;  // too far, or approaching from behind
        V3 q = closestOnTri(c, t.a, t.b, t.c);
        V3 dv = c - q;
        float dist = len(dv);
        if (dist >= r) return;
        Contact ct;
        if (sd >= 0 && dist > 1e-5f) {
            ct.n = dv * (1.0f / dist);
            ct.depth = r - dist;
        } else {
            ct.n = t.n;
            ct.depth = r - sd;
        }
        ct.point = q;
        out[n++] = ct;
    });
    return n;
}

float CollisionWorld::groundHeight(float x, float y, float fromZ) const {
    Hit h;
    if (raycast(V3(x, y, fromZ), V3(0, 0, -1), 200, h)) return fromZ - h.t;
    return 0;
}

// ---------------------------------------------------------------- car

static const float kTravel = 0.03f;   // suspension travel each way
static const float kSpringHz = 3.2f;
static const float kDampRatio = 0.55f;

void Car::setup(const std::vector<V3>& wc, float wheelRadius, const std::vector<CollSphere>& sph, const CarSpec& s) {
    spec = s;
    V3 c(0, 0, 0);
    for (auto& w : wc) c += w;
    c = c * (1.0f / wc.size());
    comModel = V3(c.x, c.y, c.z + 0.02f);
    wheels.clear();
    for (size_t i = 0; i < wc.size(); i++) {
        Wheel w;
        w.local = wc[i] - comModel;
        w.radius = wheelRadius;
        w.front = wc[i].y > c.y;
        wheels.push_back(w);
    }
    spheres.clear();
    boundRadius = 0;
    for (auto& cs : sph) {
        CollSphere o{cs.c - comModel, cs.r};
        spheres.push_back(o);
        boundRadius = std::max(boundRadius, len(o.c) + o.r);
    }
    float m = spec.mass;
    inertia_ = V3(m / 12 * (0.49f + 0.04f), m / 12 * (0.16f + 0.04f), m / 12 * (0.16f + 0.49f)) * 1.6f;
}

void Car::place(const V3& g, float heading, const CollisionWorld& w) {
    V3 f(std::cos(heading), std::sin(heading), 0);
    V3 u(0, 0, 1);
    R = M3::fromAxes(norm(cross(f, u)), f, u);
    float gz = w.groundHeight(g.x, g.y, g.z + 2.0f);
    float wheelZ = wheels.empty() ? 0.06f : -wheels[0].local.z + wheels[0].radius;
    pos = V3(g.x, g.y, gz + wheelZ + 0.01f);
    vel = V3();
    angVel = V3();
    for (auto& wh : wheels) { wh.comp = 0; wh.spin = 0; }
}

void Car::applyImpulse(const V3& at, const V3& J) {
    vel += J * (1.0f / spec.mass);
    V3 tb = R.tmul(cross(at, J));
    V3 dw(tb.x / inertia_.x, tb.y / inertia_.y, tb.z / inertia_.z);
    angVel += R * dw;
}

void Car::step(float dt, const CollisionWorld& world) {
    const float m = spec.mass;
    const float quarter = m / wheels.size();
    const float k = quarter * std::pow(2 * 3.14159f * kSpringHz, 2.0f);
    const float cdamp = 2 * kDampRatio * std::sqrt(k * quarter);
    const float preload = quarter * kGravity / k;

    V3 right = R.col(0), fwd = R.col(1), up = R.col(2);
    V3 force(0, 0, -kGravity * m);
    V3 torque(0, 0, 0);
    float fwdSpeed = dot(vel, fwd);

    // Engine: force falls off linearly towards top speed.
    float drive = 0;
    bool reversing = false;
    if (hold) {
        // parked on the grid: no drive, wheels locked
    } else if (throttle > 0.01f) {
        float sp = std::max(fwdSpeed, 0.0f);
        drive = throttle * spec.accel * m * clampf(1.0f - sp / spec.topSpeed, 0.0f, 1.0f);
    } else if (brake > 0.01f && fwdSpeed < 0.6f) {
        reversing = true;
        float sp = std::max(-fwdSpeed, 0.0f);
        drive = -brake * spec.accel * 0.6f * m * clampf(1.0f - sp / 5.0f, 0.0f, 1.0f);
    }
    int driven = 0;
    for (auto& w : wheels) if (spec.fourWD || !w.front) driven++;

    float steerAngle = -steer * spec.steerMax * clampf(1.0f - std::fabs(fwdSpeed) / (spec.topSpeed * 2.2f), 0.45f, 1.0f);
    skid = 0;
    int contacts = 0;
    for (auto& w : wheels) {
        w.steer = w.front ? steerAngle : 0;
        V3 mountL = w.local + V3(0, 0, kTravel);
        V3 mount = pos + R * mountL;
        float rayLen = kTravel + preload + w.radius + kTravel;
        Hit h;
        float prevComp = w.comp;
        w.contact = world.raycast(mount, -up, rayLen + 0.02f, h) && h.t <= rayLen && dot(h.n, up) > 0.3f;
        if (!w.contact) {
            w.comp = std::max(w.comp - dt * 0.5f, -kTravel);
            w.slip = 0;
            w.spin += fwdSpeed / w.radius * dt * 0.98f;
            continue;
        }
        contacts++;
        w.surface = h.surface;
        // comp: hub displacement from rest (positive = compressed).
        float hubDist = h.t - w.radius;          // mount -> hub
        float x = (kTravel - hubDist) + preload;  // spring compression incl. preload
        w.comp = clampf(kTravel - hubDist, -kTravel, kTravel);
        float cv = (w.comp - prevComp) / dt;
        float fs = k * x + cdamp * cv;
        if (kTravel - hubDist > kTravel) fs += (kTravel - hubDist - kTravel) * k * 8;  // bump stop
        fs = std::max(fs, 0.0f);
        V3 cp = mount - up * h.t;  // contact point
        V3 r = cp - pos;
        V3 n = h.n;
        V3 Fsusp = up * fs;

        // Tire frame on the ground plane.
        float ca = std::cos(w.steer), sa = std::sin(w.steer);
        V3 wf = norm(fwd * ca - right * sa);
        V3 wfg = norm(wf - n * dot(wf, n));
        V3 wsg = norm(cross(wfg, n));
        V3 vc = vel + cross(angVel, r);
        float vlong = dot(vc, wfg), vlat = dot(vc, wsg);
        float N = fs;
        float mu = spec.grip;
        float maxF = mu * N;
        // Lateral: cancel side slip, limited by friction.
        float latNeeded = -vlat * quarter / dt * 0.6f;
        float Flat = clampf(latNeeded, -maxF, maxF);
        float Flong = 0;
        if (spec.fourWD || !w.front) Flong += drive / driven;
        if (hold) {
            float stop = -vlong * quarter / dt;
            Flong += clampf(stop, -maxF, maxF);
        } else if (brake > 0.01f && !reversing) {
            float bf = brake * m * 9.0f / wheels.size();
            float stop = -vlong * quarter / dt;
            Flong += clampf(stop, -bf, bf);
        }
        if (handbrake && !w.front) {
            float stop = -vlong * quarter / dt;
            Flong += clampf(stop, -maxF * 0.8f, maxF * 0.8f);
            Flat *= 0.35f;
        }
        // rolling resistance
        Flong += -vlong * m * 0.12f / wheels.size();
        float tot = std::sqrt(Flat * Flat + Flong * Flong);
        if (tot > maxF && tot > 1e-6f) {
            float s = maxF / tot;
            Flat *= s;
            Flong *= s;
        }
        w.slip = clampf(std::fabs(vlat) / 2.5f, 0.0f, 1.0f);
        skid = std::max(skid, w.slip * (std::fabs(vlong) > 1.0f || std::fabs(vlat) > 1.0f ? 1.0f : 0.0f));
        w.spin += vlong / w.radius * dt;
        V3 F = Fsusp + wsg * Flat + wfg * Flong;
        force += F;
        torque += cross(r, F);
    }
    airTime = contacts == 0 ? airTime + dt : 0;

    // Aero drag and a little downforce for stability.
    float sp = len(vel);
    force += vel * (-0.02f * sp * m);
    if (contacts > 0) force += up * (-0.15f * sp * sp * m * 0.05f);

    // Arcade air control: level out gently when airborne.
    if (contacts == 0) {
        V3 corr = cross(up, V3(0, 0, 1)) * 0.6f;
        torque += corr * (m * 0.05f);
    }

    vel += force * (dt / m);
    V3 tb = R.tmul(torque);
    V3 wb = R.tmul(angVel);
    wb += V3(tb.x / inertia_.x, tb.y / inertia_.y, tb.z / inertia_.z) * dt;
    wb *= std::pow(0.4f, dt);  // angular damping
    angVel = R * wb;
    pos += vel * dt;
    R.rotate(angVel, dt);

    // Chassis spheres against the world.
    impact = 0;
    for (const CollSphere& s : spheres) {
        CollisionWorld::Contact cts[8];
        V3 r = R * s.c;
        int nc = world.sphere(pos + r, s.r, cts, 8);
        for (int i = 0; i < nc; i++) {
            const auto& c = cts[i];
            pos += c.n * (c.depth * 0.9f);
            V3 rr = c.point - pos;
            V3 vp = vel + cross(angVel, rr);
            float vn = dot(vp, c.n);
            if (vn >= 0) continue;
            V3 rn = R.tmul(cross(rr, c.n));
            V3 iw = R * V3(rn.x / inertia_.x, rn.y / inertia_.y, rn.z / inertia_.z);
            float denom = 1.0f / m + dot(c.n, cross(iw, rr));
            float j = -(1.0f + 0.25f) * vn / denom;
            V3 vt = vp - c.n * vn;
            float vtl = len(vt);
            V3 J = c.n * j;
            if (vtl > 1e-4f) J += vt * (-std::min(j * 0.3f, vtl * m * 0.5f) / vtl);
            applyImpulse(rr, J);
            impact = std::max(impact, j / m);
        }
    }

    rpm = clampf(std::fabs(fwdSpeed) / spec.topSpeed, 0.0f, 1.2f);
    if (throttle > 0.1f && contacts < 2) rpm = std::min(1.2f, rpm + 0.4f);
}

M4 Car::bodyMatrix() const { return M4::fromRT(R, pos) * M4::translate(-comModel); }

M4 Car::wheelMatrix(int i, const V3& c) const {
    const Wheel& w = wheels[i];
    return bodyMatrix() * M4::translate(c + V3(0, 0, w.comp)) * M4::rotZ(w.steer) * M4::rotX(-w.spin) * M4::translate(-c);
}

void collideCars(Car& a, Car& b) {
    V3 d = b.pos - a.pos;
    float rr = (a.boundRadius + b.boundRadius) * 0.6f;
    if (dot(d, d) > rr * rr * 4) return;
    // Two spheres per car along its forward axis.
    for (int i = -1; i <= 1; i += 2)
        for (int j = -1; j <= 1; j += 2) {
            V3 pa = a.pos + a.R.col(1) * (0.16f * i);
            V3 pb = b.pos + b.R.col(1) * (0.16f * j);
            V3 dd = pb - pa;
            float dist = len(dd);
            float r = 0.36f;
            if (dist >= r || dist < 1e-5f) continue;
            V3 n = dd * (1.0f / dist);
            float pen = r - dist;
            a.pos -= n * (pen * 0.5f);
            b.pos += n * (pen * 0.5f);
            float vn = dot(b.vel - a.vel, n);
            if (vn < 0) {
                float j = -(1.3f) * vn / (1 / a.spec.mass + 1 / b.spec.mass);
                a.vel -= n * (j / a.spec.mass);
                b.vel += n * (j / b.spec.mass);
                a.impact = std::max(a.impact, j / a.spec.mass);
                b.impact = std::max(b.impact, j / b.spec.mass);
            }
        }
}

}  // namespace bsr
