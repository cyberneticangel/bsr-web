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
    void beginFrame(int w, int h, const M4& view, const M4& proj, const V3& eye);
    // Builds GPU geometry for every mesh node. skip(name) can drop helper nodes.
    void buildModel(const FSO& fso, const std::string& texDir, Model& out,
                    const std::map<std::string, std::string>& texOverride = {});
    void drawPart(const Part& p, const M4& world, Pass pass, float alphaMul = 1.0f);
    Texture texture(const std::string& name, const std::string& dir);
    void setSun(const V3& dir, float amb, float diff) { sunDir_ = norm(dir); amb_ = amb; diff_ = diff; }
    void setFog(float r, float g, float b, float start, float end) { fog_[0] = r; fog_[1] = g; fog_[2] = b; fogStart_ = start; fogEnd_ = end; }
    void setTint(float r, float g, float b) { tint_[0] = r; tint_[1] = g; tint_[2] = b; }
    void drawLines(const std::vector<float>& xyz, float r, float g, float b);

private:
    GLuint prog_ = 0, lineProg_ = 0, lineVbo_ = 0, lineVao_ = 0, white_ = 0;
    GLint uTint_, uMVP_, uModel_, uEye_, uSun_, uAmbDiff_, uMatDiff_, uMatAmb_, uFlags_, uFog_, uFogRange_, uTex0_, uTex1_, uAlphaMul_;
    GLint lMVP_, lCol_;
    M4 viewProj_;
    V3 eye_, sunDir_{0.4f, -0.4f, 0.82f};
    float amb_ = 0.55f, diff_ = 0.6f;
    float fog_[3] = {0.7f, 0.75f, 0.8f}, fogStart_ = 60, fogEnd_ = 250;
    float tint_[3] = {1, 1, 1};
    std::map<std::string, Texture> texCache_;
};

}  // namespace bsr
