// Particle effects: rain, tire smoke, spray from wet tires, skid marks. Simulated in world space, drawn as
// camera-facing (or ground-aligned) quads through Renderer::drawSprites with the original FX_* textures.
#pragma once
#include <string>
#include <vector>

#include "mathx.h"
#include "race.h"
#include "render.h"

namespace bsr {

class Effects {
public:
    // Call when a race starts. cls: per racer, picks the tire smoke texture.
    void reset(Renderer& gfx, bool rain, bool wet, const std::vector<std::string>& cls);
    // Spawns from the cars' wheels and moves everything on. frozen: countdown/paused (nothing spawns).
    void update(float dt, const std::vector<Racer>& racers, const CollisionWorld& world, const V3& camPos);
    // Draws for one view (billboards face its camera).
    void draw(Renderer& gfx, const V3& eye, const V3& right, const V3& up);
    bool enabled = true;

private:
    struct Puff { V3 p, v; float age, life, size0, size1, alpha; int tex; float rot, spin; uint32_t tint; };
    struct Mark { V3 a0, a1, b0, b1; float age, alpha; };  // a = previous point, b = current; 0/1 = sides
    struct WheelTrail { V3 left, right; bool active = false; };
    std::vector<Puff> puffs_;
    std::vector<Mark> marks_;
    size_t markHead_ = 0;
    std::vector<std::vector<WheelTrail>> trails_;  // per racer, per wheel
    std::vector<float> emit_;                      // per racer and wheel: fractional particles owed
    std::vector<V3> drops_;
    std::vector<int> smokeTex_;                    // per racer: index into tex_
    std::vector<Texture> tex_;                     // 0 spray, 1 rain spray, then tire smoke textures
    bool rain_ = false, wet_ = false;
    uint32_t seed_ = 12345;
    float rnd() { seed_ = seed_ * 1664525u + 1013904223u; return (float)(seed_ >> 8) / 16777216.0f; }
    std::vector<SpriteVtx> buf_;
};

}  // namespace bsr
