#pragma once
#include <cmath>

struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    V3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 norm(const V3& a) { float l = len(a); return l > 1e-9f ? a * (1.0f / l) : V3(0, 0, 1); }
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// 3x3 rotation, row-major: columns are the body axes in world space (right, forward, up).
struct M3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 col(int c) const { return {m[c], m[3 + c], m[6 + c]}; }
    V3 operator*(const V3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
    V3 tmul(const V3& v) const {  // transpose * v (world -> body)
        return {m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z, m[2] * v.x + m[5] * v.y + m[8] * v.z};
    }
    static M3 fromAxes(const V3& r, const V3& f, const V3& u) {
        M3 o;
        o.m[0] = r.x; o.m[1] = f.x; o.m[2] = u.x;
        o.m[3] = r.y; o.m[4] = f.y; o.m[5] = u.y;
        o.m[6] = r.z; o.m[7] = f.z; o.m[8] = u.z;
        return o;
    }
    // Integrate angular velocity w (world) for dt and re-orthonormalize.
    void rotate(const V3& w, float dt) {
        V3 c[3] = {col(0), col(1), col(2)};
        for (auto& a : c) a += cross(w, a) * dt;
        V3 f = norm(c[1]);
        V3 r = norm(cross(f, c[2]));
        V3 u = cross(r, f);
        *this = fromAxes(r, f, u);
    }
};

// Column-major 4x4 for GL.
struct M4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    M4 operator*(const M4& b) const {
        M4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    V3 point(const V3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    static M4 perspective(float fovy, float aspect, float n, float f) {
        M4 r;
        float t = 1.0f / std::tan(fovy * 0.5f);
        r.m[0] = t / aspect; r.m[5] = t; r.m[10] = (f + n) / (n - f); r.m[11] = -1;
        r.m[14] = 2 * f * n / (n - f); r.m[15] = 0;
        return r;
    }
    static M4 lookAt(const V3& eye, const V3& at, const V3& up) {
        V3 f = norm(at - eye), s = norm(cross(f, up)), u = cross(s, f);
        M4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
        return r;
    }
    static M4 fromRT(const M3& R, const V3& t) {
        M4 o;
        o.m[0] = R.m[0]; o.m[1] = R.m[3]; o.m[2] = R.m[6];
        o.m[4] = R.m[1]; o.m[5] = R.m[4]; o.m[6] = R.m[7];
        o.m[8] = R.m[2]; o.m[9] = R.m[5]; o.m[10] = R.m[8];
        o.m[12] = t.x; o.m[13] = t.y; o.m[14] = t.z;
        return o;
    }
    static M4 translate(const V3& t) { M4 o; o.m[12] = t.x; o.m[13] = t.y; o.m[14] = t.z; return o; }
    static M4 rotX(float a) {
        M4 o; float c = std::cos(a), s = std::sin(a);
        o.m[5] = c; o.m[6] = s; o.m[9] = -s; o.m[10] = c;
        return o;
    }
    static M4 rotZ(float a) {
        M4 o; float c = std::cos(a), s = std::sin(a);
        o.m[0] = c; o.m[1] = s; o.m[4] = -s; o.m[5] = c;
        return o;
    }
};
