#include "render.h"

#include <cstdio>
#include <cstring>

namespace bsr {

static const char* kVS = R"(#version 300 es
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_nrm;
layout(location=2) in vec2 a_uv0;
layout(location=3) in vec2 a_uv1;
layout(location=4) in vec4 a_col;
uniform mat4 u_mvp, u_model;
out vec3 v_wpos, v_nrm;
out vec2 v_uv0, v_uv1;
out vec4 v_col;
void main() {
  vec4 wp = u_model * vec4(a_pos, 1.0);
  v_wpos = wp.xyz;
  v_nrm = mat3(u_model) * a_nrm;
  // Rows are uploaded bottom-up as stored on disk and UVs are 3ds Max style (v=0 at the bottom),
  // matching how the original uploaded them, so no flip is needed.
  v_uv0 = a_uv0;
  v_uv1 = a_uv1;
  v_col = a_col.zyxw;  // stored BGRA
  gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

static const char* kFS = R"(#version 300 es
precision highp float;
in vec3 v_wpos, v_nrm;
in vec2 v_uv0, v_uv1;
in vec4 v_col;
uniform sampler2D u_tex0, u_tex1;
uniform vec3 u_eye, u_sun, u_fog;
uniform vec2 u_ambdiff, u_fogRange;
uniform vec4 u_matDiff, u_matAmb;
uniform vec4 u_flags;   // lightmap, envmap, unlit, vertexcolor
uniform vec3 u_tint;
uniform vec2 u_alpha;   // x: alpha-test threshold, y: alpha multiplier
out vec4 o_col;
void main() {
  vec4 c = texture(u_tex0, v_uv0);
  if (c.a < u_alpha.x) discard;
  vec3 n = normalize(v_nrm);
  if (!gl_FrontFacing) n = -n;
  vec3 light;
  if (u_flags.z > 0.5) {
    light = u_tint;
  } else {
    float nd = max(dot(n, u_sun), 0.0);
    light = u_matAmb.rgb * u_ambdiff.x + u_matDiff.rgb * u_ambdiff.y * nd;
  }
  if (u_flags.w > 0.5) light *= v_col.rgb;
  c.rgb *= light;
  if (u_flags.x > 0.5) c.rgb *= texture(u_tex1, v_uv1).rgb;
  if (u_flags.y > 0.5) {
    vec3 v = normalize(v_wpos - u_eye);
    vec3 r = reflect(v, n);
    vec2 sm = r.xy / (2.0 * sqrt(r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0))) + 0.5;
    c.rgb += texture(u_tex1, sm).rgb * 0.25;
    if (u_flags.z < 0.5) {
      vec3 h = normalize(u_sun - v);
      c.rgb += vec3(pow(max(dot(n, h), 0.0), 40.0) * 0.5);
    }
  }
  float d = length(v_wpos - u_eye);
  float f = clamp((d - u_fogRange.x) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
  c.rgb = mix(c.rgb, u_fog, f * 0.85);
  c.a *= u_alpha.y * u_matDiff.a;
  o_col = c;
}
)";

static const char* kLineVS = R"(#version 300 es
layout(location=0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";
static const char* kLineFS = R"(#version 300 es
precision mediump float;
uniform vec3 u_col;
out vec4 o;
void main() { o = vec4(u_col, 1.0); }
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

static GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        fprintf(stderr, "link: %s\n", log);
    }
    return p;
}

Part* Model::find(const std::string& n) {
    for (auto& p : parts)
        if (p.name == n) return &p;
    return nullptr;
}

bool Renderer::init() {
    prog_ = link(kVS, kFS);
    uMVP_ = glGetUniformLocation(prog_, "u_mvp");
    uModel_ = glGetUniformLocation(prog_, "u_model");
    uEye_ = glGetUniformLocation(prog_, "u_eye");
    uSun_ = glGetUniformLocation(prog_, "u_sun");
    uAmbDiff_ = glGetUniformLocation(prog_, "u_ambdiff");
    uMatDiff_ = glGetUniformLocation(prog_, "u_matDiff");
    uMatAmb_ = glGetUniformLocation(prog_, "u_matAmb");
    uFlags_ = glGetUniformLocation(prog_, "u_flags");
    uFog_ = glGetUniformLocation(prog_, "u_fog");
    uFogRange_ = glGetUniformLocation(prog_, "u_fogRange");
    uTex0_ = glGetUniformLocation(prog_, "u_tex0");
    uTex1_ = glGetUniformLocation(prog_, "u_tex1");
    uAlphaMul_ = glGetUniformLocation(prog_, "u_alpha");
    uTint_ = glGetUniformLocation(prog_, "u_tint");
    lineProg_ = link(kLineVS, kLineFS);
    lMVP_ = glGetUniformLocation(lineProg_, "u_mvp");
    lCol_ = glGetUniformLocation(lineProg_, "u_col");
    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, 0);
    glBindVertexArray(0);
    uint32_t w = 0xffffffff;
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &w);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    return true;
}

Texture Renderer::texture(const std::string& name, const std::string& dir) {
    std::string key = dir + "/" + name;
    auto it = texCache_.find(key);
    if (it != texCache_.end()) return it->second;
    Texture t;
    Image img;
    if (loadTexture(name, dir, img)) {
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE /*MAX_ANISOTROPY_EXT*/, 8.0f);
        glGetError();  // anisotropy may be unsupported
        t.alpha = img.hasAlpha;
    } else {
        fprintf(stderr, "missing texture %s\n", name.c_str());
    }
    texCache_[key] = t;
    return t;
}

struct Vtx {
    float p[3], n[3], uv0[2], uv1[2];
    uint32_t col;
};

static bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static void collect(const Node& n, const M4& parent, std::vector<std::pair<const Node*, M4>>& out) {
    M4 m = parent;
    if (n.type == 2) {
        M4 l;
        memcpy(l.m, n.matrix, 64);
        m = parent * l;
    }
    std::string ln = lower(n.name);
    if (ln.rfind("g_reflection", 0) == 0 || ln.find("sunflare") != std::string::npos) return;
    if (n.type == 6) out.push_back({&n, m});
    for (auto& c : n.children) collect(c, m, out);
}

void Renderer::buildModel(const FSO& fso, const std::string& texDir, Model& out,
                          const std::map<std::string, std::string>& texOverride) {
    std::vector<std::pair<const Node*, M4>> nodes;
    collect(fso.root, M4(), nodes);
    bool isTrack = !texDir.empty();
    for (auto& [node, xf] : nodes) {
        if (node->geoset < 0 || node->geoset >= (int)fso.geosets.size()) continue;
        const Geoset& gs = fso.geosets[node->geoset];
        if (gs.lods.empty()) continue;
        const Mesh& m = gs.lods[0];
        if (m.idx.empty()) continue;
        const std::vector<std::string>* mt =
            node->mattable >= 0 && node->mattable < (int)fso.mattables.size() ? &fso.mattables[node->mattable] : nullptr;
        Part part;
        part.name = node->name;
        part.local = xf;
        part.noFog = lower(node->name).find("dome") != std::string::npos;
        std::vector<Vtx> vtx(m.pos.size());
        V3 bmin(1e9f, 1e9f, 1e9f), bmax(-1e9f, -1e9f, -1e9f);
        for (size_t i = 0; i < vtx.size(); i++) {
            Vtx& v = vtx[i];
            memcpy(v.p, &m.pos[i], 12);
            bmin = {std::fmin(bmin.x, v.p[0]), std::fmin(bmin.y, v.p[1]), std::fmin(bmin.z, v.p[2])};
            bmax = {std::fmax(bmax.x, v.p[0]), std::fmax(bmax.y, v.p[1]), std::fmax(bmax.z, v.p[2])};
            if (!m.nrm.empty()) memcpy(v.n, &m.nrm[i], 12); else { v.n[0] = 0; v.n[1] = 0; v.n[2] = 1; }
            if (!m.uv0.empty()) memcpy(v.uv0, &m.uv0[i * 2], 8); else v.uv0[0] = v.uv0[1] = 0;
            if (!m.uv1.empty()) memcpy(v.uv1, &m.uv1[i * 2], 8); else { v.uv1[0] = v.uv0[0]; v.uv1[1] = v.uv0[1]; }
            v.col = m.col.empty() ? 0xffffffff : m.col[i];
        }
        part.bmin = bmin;
        part.bmax = bmax;
        // Sort triangles by material slot.
        size_t nt = m.idx.size() / 3;
        std::map<int, std::vector<uint16_t>> byMat;
        for (size_t t = 0; t < nt; t++) {
            int mi = m.faceMat.empty() ? 0 : m.faceMat[t];
            auto& v = byMat[mi];
            v.insert(v.end(), &m.idx[t * 3], &m.idx[t * 3 + 3]);
        }
        std::vector<uint16_t> idx;
        GLuint vao, vbo, ibo;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, vtx.size() * sizeof(Vtx), vtx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, p));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, n));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, uv0));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, uv1));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, col));
        for (auto& [mi, tris] : byMat) {
            Batch b;
            b.vao = vao;
            b.first = (GLsizei)idx.size();
            b.count = (GLsizei)tris.size();
            idx.insert(idx.end(), tris.begin(), tris.end());
            const Material* mat = nullptr;
            if (mt && mi < (int)mt->size()) mat = fso.findMaterial((*mt)[mi]);
            b.unlit = isTrack;
            b.hasColor = !m.col.empty();
            if (mat) {
                memcpy(b.diffuse, mat->diffuse, 16);
                memcpy(b.ambient, mat->ambient, 16);
                if (!mat->textures.empty()) {
                    std::string t0 = mat->textures[0];
                    auto ov = texOverride.find(t0);
                    if (ov != texOverride.end()) t0 = ov->second;
                    b.tex0 = texture(t0, texDir);
                }
                if (mat->textures.size() > 1) {
                    const std::string& t1 = mat->textures[1];
                    b.tex1 = texture(t1, texDir);
                    if (contains(t1, "environment")) b.envmap = true;
                    else b.lightmap = true;
                }
                std::string ln = lower(mat->name) + " " + (mat->textures.empty() ? "" : mat->textures[0]);
                b.blend = contains(ln, "schaduw") || contains(ln, "shadow") || contains(ln, "fx_");
                b.twoSided = b.tex0.alpha;
            }
            if (!b.tex0.id) b.tex0.id = white_;
            if (!b.tex1.id) { b.tex1.id = white_; b.lightmap = b.envmap = false; }
            part.batches.push_back(b);
        }
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 2, idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        out.parts.push_back(std::move(part));
    }
}

void Renderer::beginFrame(int w, int h, const M4& view, const M4& proj, const V3& eye) {
    viewProj_ = proj * view;
    eye_ = eye;
    glViewport(0, 0, w, h);
    glClearColor(fog_[0], fog_[1], fog_[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glUseProgram(prog_);
    glUniform3f(uEye_, eye.x, eye.y, eye.z);
    glUniform3f(uSun_, sunDir_.x, sunDir_.y, sunDir_.z);
    glUniform2f(uAmbDiff_, amb_, diff_);
    glUniform3f(uFog_, fog_[0], fog_[1], fog_[2]);
    glUniform2f(uFogRange_, fogStart_, fogEnd_);
    glUniform3f(uTint_, tint_[0], tint_[1], tint_[2]);
    glUniform1i(uTex0_, 0);
    glUniform1i(uTex1_, 1);
}

void Renderer::drawPart(const Part& p, const M4& world, Pass pass, float alphaMul) {
    M4 model = world * p.local;
    M4 mvp = viewProj_ * model;
    glUseProgram(prog_);
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.m);
    glUniform2f(uFogRange_, p.noFog ? 1e6f : fogStart_, p.noFog ? 2e6f : fogEnd_);
    for (const Batch& b : p.batches) {
        bool isBlend = b.blend || alphaMul < 1.0f;
        if ((pass == Pass::Blend) != isBlend) continue;
        if (isBlend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1, -2);
        }
        if (b.twoSided || isBlend) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, b.tex0.id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, b.tex1.id);
        glUniform4f(uFlags_, b.lightmap, b.envmap, b.unlit, b.hasColor);
        glUniform4fv(uMatDiff_, 1, b.diffuse);
        glUniform4fv(uMatAmb_, 1, b.ambient);
        glUniform2f(uAlphaMul_, isBlend ? 0.01f : (b.tex0.alpha ? 0.5f : -1.0f), alphaMul);
        glBindVertexArray(b.vao);
        glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_SHORT, (void*)(intptr_t)(b.first * 2));
        if (isBlend) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    glBindVertexArray(0);
}

void Renderer::drawLines(const std::vector<float>& xyz, float r, float g, float b) {
    if (xyz.empty()) return;
    glUseProgram(lineProg_);
    glUniformMatrix4fv(lMVP_, 1, GL_FALSE, viewProj_.m);
    glUniform3f(lCol_, r, g, b);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, xyz.size() * 4, xyz.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, (GLsizei)xyz.size() / 3);
    glBindVertexArray(0);
}

}  // namespace bsr
