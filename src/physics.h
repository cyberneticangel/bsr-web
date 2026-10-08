#pragma once
#include <vector>

#include "formats.h"
#include "mathx.h"

namespace bsr {

struct Hit {
    float t = 0;
    V3 n;
    uint32_t surface = 0;
};

// Static collision geometry from the track .fst, bucketed in a 2D grid for queries.
class CollisionWorld {
public:
    void build(const FST& fst);
    bool raycast(const V3& o, const V3& d, float maxT, Hit& hit) const;
    // Returns pushout contacts for a sphere.
    struct Contact { V3 n; float depth; V3 point; };
    int sphere(const V3& c, float r, Contact* out, int maxOut) const;
    float groundHeight(float x, float y, float fromZ) const;
    V3 bmin, bmax;

private:
    struct Tri { V3 a, b, c, n; uint32_t surface; V3 lo, hi; };
    std::vector<Tri> tris_;
    std::vector<std::vector<uint32_t>> cells_;
    float cell_ = 2.0f;
    int nx_ = 0, ny_ = 0;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t frame_ = 0;
    template <typename F> void query(float x0, float y0, float x1, float y1, F&& f) const;
};

struct CarSpec {
    float topSpeed = 15;      // m/s
    float accel = 9;          // m/s^2 at low speed
    float grip = 1.25f;       // friction coefficient
    float mass = 8;
    float steerMax = 0.45f;   // rad
    bool fourWD = true;
};

struct Wheel {
    V3 local;        // hub centre at rest, body coords relative to CoM
    float radius = 0.06f;
    bool front = false;
    // state
    float comp = 0, spin = 0, steer = 0;
    bool contact = false;
    float slip = 0;
    uint32_t surface = 0;
};

struct CollSphere { V3 c; float r; };

class Car {
public:
    void setup(const std::vector<V3>& wheelCentres, float wheelRadius, const std::vector<CollSphere>& spheres,
               const CarSpec& spec);
    void place(const V3& groundPos, float heading, const CollisionWorld& w);
    void step(float dt, const CollisionWorld& w);
    M4 bodyMatrix() const;  // model-space -> world (model origin, not CoM)
    M4 wheelMatrix(int i, const V3& modelWheelCentre) const;
    float speed() const { return dot(vel, R.col(1)); }

    // inputs
    float throttle = 0, brake = 0, steer = 0;  // steer -1..1 (right positive)
    bool handbrake = false;
    bool hold = false;  // parked (countdown)

    V3 pos, vel, angVel;  // pos is CoM
    M3 R;
    V3 comModel;          // CoM in model coords
    std::vector<Wheel> wheels;
    std::vector<CollSphere> spheres;  // relative to CoM
    CarSpec spec;
    float impact = 0;     // strongest collision impulse this step (for audio)
    float skid = 0;       // 0..1
    float rpm = 0;        // 0..1 engine sound pitch driver
    float airTime = 0;
    float boundRadius = 0.4f;

private:
    V3 inertia_;  // body-frame diagonal
    void applyImpulse(const V3& at, const V3& J);
};

// moveA/moveB false: that car is only an obstacle (its state comes from elsewhere, e.g. the network).
void collideCars(Car& a, Car& b, bool moveA = true, bool moveB = true);

}  // namespace bsr
