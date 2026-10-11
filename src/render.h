#pragma once
#include <GLES3/gl3.h>

#include <map>
#include <string>
#include <vector>

#include "formats.h"
#include "mathx.h"

namespace bsr {

struct Texture {
    GLuint id = 0;
    bool alpha = false;
};

enum class Pass { Opaque, Blend };

// Particles, rain and skid marks: world-space quads built on the CPU every frame (4 vertices each).
struct SpriteVtx {
    float p[3], uv[2];
    uint32_t rgba;  // bytes r, g, b, a
};

struct Batch {
    GLuint vao = 0;
    GLsizei first = 0, count = 0;
    Texture tex0, tex1;
    bool lightmap = false, envmap = false, unlit = false, blend = false, twoSided = false, hasColor = false;
    float diffuse[4] = {1, 1, 1, 1};
    float ambient[4] = {1, 1, 1, 1};
};

struct Part {  // a mesh node, LOD 0
    std::string name;
    GLuint vao = 0, vbo = 0, ibo = 0;  // owned; freed by Renderer::release
    M4 local;  // accumulated transform within the model
    std::vector<Batch> batches;
    V3 bmin, bmax;
    bool noFog = false;
};

struct Model {
    std::vector<Part> parts;
    Part* find(const std::string& n);
};

class Renderer {
public:
    bool init();
    // Clears and draws into the viewport (x, y, w, h); several per frame for split-screen.
    void beginFrame(int x, int y, int w, int h, const M4& view, const M4& proj, const V3& eye);
    // Builds GPU geometry for every mesh node. skip(name) can drop helper nodes.
    void buildModel(const FSO& fso, const std::string& texDir, Model& out,
                    const std::map<std::string, std::string>& texOverride = {});
    // Frees the model's GPU buffers (textures stay in the cache) and empties it.
    void release(Model& m);
    void drawPart(const Part& p, const M4& world, Pass pass, float alphaMul = 1.0f);
    Texture texture(const std::string& name, const std::string& dir);
    void setSun(const V3& dir, float amb, float diff) { sunDir_ = norm(dir); amb_ = amb; diff_ = diff; }
    void setFog(float r, float g, float b, float start, float end) { fog_[0] = r; fog_[1] = g; fog_[2] = b; fogStart_ = start; fogEnd_ = end; }
    void setTint(float r, float g, float b) { tint_[0] = r; tint_[1] = g; tint_[2] = b; }
    void drawLines(const std::vector<float>& xyz, float r, float g, float b);
    // Alpha-blended, fogged, depth-tested without depth writes. quads: 4 vertices each (a, b, c, d around).
    // decal: pulled towards the camera so marks on the ground don't z-fight with it.
    void drawSprites(const std::vector<SpriteVtx>& quads, const Texture& tex, bool decal = false);
    Texture white() const { return Texture{white_, false}; }
    // Offscreen RGBA target (menu previews): begin binds it, end reads it back (rows bottom-up) and unbinds.
    void beginOffscreen(int w, int h);
    void endOffscreen(std::vector<uint8_t>& rgba);
    void clearColor(float r, float g, float b, float a);
    bool fogOff = false;  // draw without fog

private:
    GLuint prog_ = 0, lineProg_ = 0, lineVbo_ = 0, lineVao_ = 0, white_ = 0;
    GLint uTint_, uMVP_, uModel_, uEye_, uSun_, uAmbDiff_, uMatDiff_, uMatAmb_, uFlags_, uFog_, uFogRange_, uTex0_, uTex1_, uAlphaMul_;
    GLint lMVP_, lCol_;
    GLuint spriteProg_ = 0, spriteVao_ = 0, spriteVbo_ = 0, spriteIbo_ = 0;
    GLint sVP_, sEye_, sFog_, sFogRange_, sTex_;
    size_t spriteQuadCap_ = 0;
    M4 viewProj_;
    V3 eye_, sunDir_{0.4f, -0.4f, 0.82f};
    float amb_ = 0.55f, diff_ = 0.6f;
    float fog_[3] = {0.7f, 0.75f, 0.8f}, fogStart_ = 60, fogEnd_ = 250;
    float tint_[3] = {1, 1, 1};
    std::map<std::string, Texture> texCache_;
    GLuint offFbo_ = 0, offCol_ = 0, offDep_ = 0;
    int offW_ = 0, offH_ = 0;
};

}  // namespace bsr
