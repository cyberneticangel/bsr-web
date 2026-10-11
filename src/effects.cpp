#include "effects.h"

#include <algorithm>
#include <cmath>

namespace bsr {

static const size_t kMaxPuffs = 3000, kMaxMarks = 4000, kDrops = 1400;
static const float kMarkLife = 40, kMarkFade = 10;
static const float kRainBox = 9, kRainHeight = 7;

static uint32_t rgba(int r, int g, int b, float a) {
    int ai = (int)std::lround(clampf(a, 0, 1) * 255);
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)ai << 24;
}

// FX_tiresmoke_<class> texture names: junior -> jun, trained -> trn, skilled -> skl, expert -> exp, pro -> pro.
static std::string smokeTexture(const std::string& cls) {
    static const char* map[][2] = {{"junior", "jun"}, {"trained", "trn"}, {"skilled", "skl"}, {"expert", "exp"}, {"pro", "pro"}};
    std::string kind = cls.find("hop") != std::string::npos ? "hop" : "std";
    for (auto& m : map)
        if (cls.rfind(m[0], 0) == 0) return std::string("fx_tiresmoke_") + m[1] + "_" + kind;
    return "fx_tiresmoke_exp_std";  // Monster
}

void Effects::reset(Renderer& gfx, bool rain, bool wet, const std::vector<std::string>& cls) {
    rain_ = rain;
    wet_ = wet;
    puffs_.clear();
    marks_.clear();
    markHead_ = 0;
    trails_.assign(cls.size(), std::vector<WheelTrail>(4));
    emit_.assign(cls.size() * 4, 0.0f);
    tex_.clear();
    tex_.push_back(gfx.texture("fx_spray_wet", ""));
    tex_.push_back(gfx.texture("fx_spray_rain", ""));
    std::vector<std::string> names;
    smokeTex_.clear();
    for (const std::string& c : cls) {
        std::string n = smokeTexture(c);
        auto it = std::find(names.begin(), names.end(), n);
        if (it == names.end()) {
            names.push_back(n);
            tex_.push_back(gfx.texture(n, ""));
            it = names.end() - 1;
        }
        smokeTex_.push_back(2 + (int)(it - names.begin()));
    }
    drops_.clear();
    if (rain_)
        for (size_t i = 0; i < kDrops; i++)
            drops_.push_back(V3((rnd() * 2 - 1) * kRainBox, (rnd() * 2 - 1) * kRainBox, (rnd() * 2 - 1) * kRainHeight));
}

void Effects::update(float dt, const std::vector<Racer>& racers, const CollisionWorld&, const V3& camPos) {
    if (!enabled || dt <= 0) return;
    if (trails_.size() < racers.size()) trails_.resize(racers.size(), std::vector<WheelTrail>(4));
    if (emit_.size() < racers.size() * 4) emit_.resize(racers.size() * 4, 0.0f);
    for (size_t i = 0; i < racers.size(); i++) {
        const Racer& r = racers[i];
        const Car& c = r.car;
        if (r.gone) continue;
        V3 right = c.R.col(0), fwd = c.R.col(1), up = c.R.col(2);
        float speed = len(c.vel);
        for (size_t k = 0; k < c.wheels.size() && k < 4; k++) {
            const Wheel& w = c.wheels[k];
            WheelTrail& tr = trails_[i][k];
            V3 hub = c.pos + c.R * (w.local + V3(0, 0, w.comp));
            V3 ground = hub - up * w.radius;
            float& owed = emit_[i * 4 + k];
            // Skid marks: a ribbon per wheel while it slides.
            bool marking = w.contact && w.slide > 0.3f && speed > 0.8f && !wet_;
            if (marking) {
                float half = std::max(0.018f, w.radius * 0.35f);
                V3 l = ground - right * half + up * 0.003f, rr = ground + right * half + up * 0.003f;
                if (!tr.active) {
                    tr.left = l;
                    tr.right = rr;
                    tr.active = true;
                } else if (len(l - tr.left) > 0.05f) {
                    Mark m{tr.left, tr.right, l, rr, 0, 0.55f * w.slide};
                    if (marks_.size() < kMaxMarks) marks_.push_back(m);
                    else { marks_[markHead_] = m; markHead_ = (markHead_ + 1) % kMaxMarks; }
                    tr.left = l;
                    tr.right = rr;
                }
            } else {
                tr.active = false;
            }
            if (!w.contact || puffs_.size() >= kMaxPuffs) { owed = 0; continue; }
            if (wet_) {  // spray thrown up behind the tires
                if (speed < 1.5f) { owed = 0; continue; }
                owed += dt * clampf(speed / 8.0f, 0, 1) * (rain_ ? 26.0f : 14.0f);
                for (; owed >= 1; owed -= 1) {
                    Puff p;
                    p.p = ground + up * 0.02f - fwd * 0.04f;
                    p.v = c.vel * 0.55f - fwd * (speed * 0.15f) + up * (0.5f + rnd() * 0.7f) + right * ((rnd() - 0.5f) * 0.8f);
                    p.age = 0;
                    p.life = 0.35f + rnd() * 0.25f;
                    p.size0 = 0.03f;
                    p.size1 = 0.16f + w.radius;
                    p.alpha = rain_ ? 0.45f : 0.3f;
                    p.tex = rain_ ? 1 : 0;
                    p.rot = rnd() * 6.283f;
                    p.spin = (rnd() - 0.5f) * 2;
                    p.tint = rgba(225, 230, 235, 1);
                    puffs_.push_back(p);
                }
            } else {  // tire smoke while sliding or locked
                if (w.slide < 0.35f || speed < 1.2f) { owed = 0; continue; }
                owed += dt * w.slide * 34.0f;
                for (; owed >= 1; owed -= 1) {
                    Puff p;
                    p.p = ground + up * 0.03f;
                    p.v = c.vel * 0.12f + up * (0.15f + rnd() * 0.2f) + V3(rnd() - 0.5f, rnd() - 0.5f, 0) * 0.3f;
                    p.age = 0;
                    p.life = 0.8f + rnd() * 0.6f;
                    p.size0 = 0.05f;
                    p.size1 = 0.3f + rnd() * 0.15f + w.radius;
                    p.alpha = 0.45f * w.slide + 0.2f;
                    p.tex = smokeTex_.empty() ? 2 : smokeTex_[i % smokeTex_.size()];
                    p.rot = rnd() * 6.283f;
                    p.spin = (rnd() - 0.5f) * 1.5f;
                    p.tint = rgba(235, 235, 235, 1);
                    puffs_.push_back(p);
                }
            }
        }
    }
    // Move.
    size_t n = 0;
    for (Puff& p : puffs_) {
        p.age += dt;
        if (p.age >= p.life) continue;
        if (wet_) p.v.z -= 5.0f * dt;  // spray falls back; smoke drifts
        p.v = p.v * std::pow(wet_ ? 0.3f : 0.15f, dt);
        p.p += p.v * dt;
        p.rot += p.spin * dt;
        puffs_[n++] = p;
    }
    puffs_.resize(n);
    for (Mark& m : marks_) m.age += dt;
    // Rain: a box of drops that follows the camera, wrapping around it.
    for (V3& d : drops_) {
        d += V3(0.7f, 0.3f, -8.0f) * dt;
        V3 rel = d - camPos;
        auto wrap = [](float v, float h) { return v < -h ? v + 2 * h : v > h ? v - 2 * h : v; };
        d = camPos + V3(wrap(rel.x, kRainBox), wrap(rel.y, kRainBox), wrap(rel.z, kRainHeight));
    }
}

void Effects::draw(Renderer& gfx, const V3& eye, const V3& right, const V3& up) {
    if (!enabled) return;
    auto quad = [&](const V3& a, const V3& b, const V3& c, const V3& d, uint32_t col) {
        buf_.push_back({{a.x, a.y, a.z}, {0, 0}, col});
        buf_.push_back({{b.x, b.y, b.z}, {1, 0}, col});
        buf_.push_back({{c.x, c.y, c.z}, {1, 1}, col});
        buf_.push_back({{d.x, d.y, d.z}, {0, 1}, col});
    };
    // Skid marks on the ground.
    buf_.clear();
    for (const Mark& m : marks_) {
        if (m.age > kMarkLife) continue;
        float a = m.alpha * clampf((kMarkLife - m.age) / kMarkFade, 0, 1);
        quad(m.a0, m.a1, m.b1, m.b0, rgba(22, 20, 18, a));
    }
    gfx.drawSprites(buf_, gfx.white(), true);
    // Smoke and spray, one batch per texture.
    for (size_t t = 0; t < tex_.size(); t++) {
        buf_.clear();
        for (const Puff& p : puffs_) {
            if (p.tex != (int)t) continue;
            float k = p.age / p.life;
            float s = p.size0 + (p.size1 - p.size0) * std::sqrt(k);
            float a = p.alpha * (1 - k) * std::min(1.0f, k * 8);
            float cr = std::cos(p.rot) * s, sr = std::sin(p.rot) * s;
            V3 ax = right * cr + up * sr, ay = up * cr - right * sr;
            uint32_t col = (p.tint & 0xffffff) | rgba(0, 0, 0, a);
            quad(p.p - ax - ay, p.p + ax - ay, p.p + ax + ay, p.p - ax + ay, col);
        }
        gfx.drawSprites(buf_, tex_[t]);
    }
    // Rain streaks: thin quads along the fall direction, facing the camera.
    if (!drops_.empty()) {
        buf_.clear();
        V3 fall = norm(V3(0.7f, 0.3f, -8.0f));
        for (const V3& d : drops_) {
            V3 side = cross(fall, d - eye);
            float l = len(side);
            if (l < 1e-4f) continue;
            side = side * (0.009f / l);
            V3 tail = d - fall * 0.45f;
            quad(d - side, d + side, tail + side, tail - side, rgba(214, 220, 230, 0.5f));
        }
        gfx.drawSprites(buf_, gfx.white());
    }
}

}  // namespace bsr
