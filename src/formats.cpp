#include "formats.h"

#include <cstdio>
#include <cstring>

namespace bsr {

static const uint8_t kKey[16] = {0x47, 0xc3, 0xf5, 0x12, 0x38, 0xe9, 0xb5, 0x91,
                                 0x25, 0x63, 0x06, 0xd9, 0xaa, 0x6f, 0x3a, 0x73};

void xorDecrypt(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] ^= kKey[i & 15];
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string baseName(const std::string& path) {
    std::string s = path;
    for (auto& c : s) if (c == '\\') c = '/';
    size_t sl = s.rfind('/');
    if (sl != std::string::npos) s = s.substr(sl + 1);
    size_t dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return lower(s);
}

std::string gDataRoot = "/data/";

bool readFile(const std::string& path, Bytes& out) {
    std::string p = gDataRoot + lower(path);
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    fclose(f);
    return got == (size_t)n;
}

template <typename T>
static T rd(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

// ---------------------------------------------------------------- textures

// TGA-style RLE: 0x80|(n-1) = repeat next pixel n times, else n literal pixels.
static bool rleDecode(const uint8_t* src, size_t srcLen, size_t npix, int bpp, std::vector<uint8_t>& out) {
    out.resize(npix * bpp);
    size_t o = 0, i = 0, total = npix * bpp;
    while (o < total && i < srcLen) {
        uint8_t c = src[i++];
        size_t n = (c & 0x7f) + 1;
        if (c & 0x80) {
            if (i + bpp > srcLen) return false;
            for (size_t k = 0; k < n && o < total; k++, o += bpp) memcpy(&out[o], &src[i], bpp);
            i += bpp;
        } else {
            size_t bytes = n * bpp;
            if (i + bytes > srcLen) bytes = srcLen - i;
            if (o + bytes > total) bytes = total - o;
            memcpy(&out[o], &src[i], bytes);
            o += bytes;
            i += n * bpp;
        }
    }
    return o == total;
}

bool decodeFSP(const Bytes& d, Image& img) {
    if (d.size() < 20) return false;
    uint32_t w = rd<uint32_t>(&d[0]), h = rd<uint32_t>(&d[4]), fmt = rd<uint32_t>(&d[8]);
    uint32_t size = rd<uint32_t>(&d[16]);
    size_t off = 20;
    const uint8_t* pal = nullptr;
    if (fmt == 1) {  // 8-bit paletted, plaintext BGRA palette precedes data
        pal = &d[20];
        off = 20 + 1024;
    }
    if (off + size > d.size()) return false;
    std::vector<uint8_t> payload(d.begin() + off, d.begin() + off + size);
    xorDecrypt(payload.data(), payload.size());
    int bpp;
    switch (fmt) {
        case 1: bpp = 1; break;
        case 2: case 4: case 5: bpp = 2; break;
        case 7: bpp = 3; break;
        case 11: bpp = 4; break;
        default: return false;
    }
    std::vector<uint8_t> px;
    if (!rleDecode(payload.data(), payload.size(), (size_t)w * h, bpp, px)) return false;
    img.w = w; img.h = h;
    img.rgba.resize((size_t)w * h * 4);
    img.hasAlpha = false;
    uint8_t* o = img.rgba.data();
    for (size_t i = 0; i < (size_t)w * h; i++, o += 4) {
        uint8_t r, g, b, a = 255;
        if (fmt == 1) {
            const uint8_t* e = pal + px[i] * 4;
            b = e[0]; g = e[1]; r = e[2]; a = e[3];
        } else if (fmt == 7) {
            b = px[i * 3]; g = px[i * 3 + 1]; r = px[i * 3 + 2];
        } else if (fmt == 11) {
            b = px[i * 4]; g = px[i * 4 + 1]; r = px[i * 4 + 2]; a = px[i * 4 + 3];
        } else {
            uint16_t v = px[i * 2] | (px[i * 2 + 1] << 8);
            if (fmt == 2) {  // RGB565
                r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31;
            } else if (fmt == 5) {  // ARGB4444
                a = (v >> 12) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17;
            } else {  // ARGB1555
                a = (v >> 15) ? 255 : 0; r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31;
            }
        }
        o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        if (a != 255) img.hasAlpha = true;
    }
    // Paletted maps (shadow/spray) carry intensity in the palette; alpha of 255 everywhere.
    return true;
}

bool decodeTGA(const Bytes& d, Image& img) {
    if (d.size() < 18) return false;
    int idLen = d[0], type = d[2];
    int w = rd<uint16_t>(&d[12]), h = rd<uint16_t>(&d[14]), bits = d[16], desc = d[17];
    if (bits != 24 && bits != 32) return false;
    int bpp = bits / 8;
    size_t off = 18 + idLen;
    std::vector<uint8_t> px;
    if (type == 2) {
        if (off + (size_t)w * h * bpp > d.size()) return false;
        px.assign(d.begin() + off, d.begin() + off + (size_t)w * h * bpp);
    } else if (type == 10) {
        if (!rleDecode(&d[off], d.size() - off, (size_t)w * h, bpp, px)) return false;
    } else {
        return false;
    }
    img.w = w; img.h = h;
    img.rgba.resize((size_t)w * h * 4);
    img.hasAlpha = false;
    bool topDown = desc & 0x20;
    for (int y = 0; y < h; y++) {
        int sy = topDown ? (h - 1 - y) : y;
        for (int x = 0; x < w; x++) {
            const uint8_t* s = &px[((size_t)sy * w + x) * bpp];
            uint8_t* o = &img.rgba[((size_t)y * w + x) * 4];
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = bpp == 4 ? s[3] : 255;
            if (o[3] != 255) img.hasAlpha = true;
        }
    }
    return true;
}

bool loadTexture(const std::string& name, const std::string& trackDir, Image& img) {
    std::string b = baseName(name);
    Bytes d;
    if (!trackDir.empty() && readFile("maps_high/" + trackDir + "/" + b + ".fsp", d)) return decodeFSP(d, img);
    if (readFile("maps_high/" + b + ".fsp", d)) return decodeFSP(d, img);
    if (readFile("maps_cars/" + b + ".tga", d)) return decodeTGA(d, img);
    return false;
}

// ---------------------------------------------------------------- FSO

const Material* FSO::findMaterial(const std::string& n) const {
    for (auto& m : materials)
        if (m.name == n) return &m;
    return nullptr;
}

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t o = 0;
    bool ok = true;
    bool need(size_t k) {
        if (o + k > n) ok = false;
        return ok;
    }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = rd<uint32_t>(p + o); o += 4; return v; }
    float f32() { if (!need(4)) return 0; float v = rd<float>(p + o); o += 4; return v; }
    std::string cstr() {
        size_t s = o;
        while (o < n && p[o]) o++;
        std::string r((const char*)p + s, o - s);
        o++;
        return r;
    }
    void align4() { o = (o + 3) & ~size_t(3); }
};

static bool parseNode(Reader& r, Node& n, FSO& fso) {
    if (!r.need(0x3c)) return false;
    size_t h = r.o;
    n.type = rd<uint32_t>(r.p + h);
    const char* nm = (const char*)r.p + h + 9;
    n.name = std::string(nm, strnlen(nm, 0x2c - 9));
    r.o += 0x3c;
    auto child = [&]() -> bool {
        n.children.emplace_back();
        return parseNode(r, n.children.back(), fso);
    };
    switch (n.type) {
        case 1: {
            if (!r.need(1)) return false;
            uint8_t has = r.p[r.o++];
            if (has && !child()) return false;
            break;
        }
        case 2: {
            if (!r.need(0x44)) return false;
            uint8_t has = r.p[r.o + 1];
            memcpy(n.matrix, r.p + r.o + 4, 64);
            r.o += 0x44;
            if (has && !child()) return false;
            break;
        }
        case 4: {  // light
            if (!r.need(0x44)) return false;
            uint8_t has = r.p[r.o];
            float f[16];
            memcpy(f, r.p + r.o + 4, 64);
            if (!fso.sun.valid) {
                fso.sun.valid = true;
                fso.sun.dir = {f[1], f[2], f[3]};
                memcpy(fso.sun.diffuse, f + 4, 16);
                memcpy(fso.sun.ambient, f + 8, 16);
            }
            r.o += 0x44;
            if (has && !child()) return false;
            break;
        }
        case 3: {
            uint32_t cnt = r.u32();
            for (uint32_t i = 0; i < cnt && r.ok; i++)
                if (!child()) return false;
            break;
        }
        case 5: {
            uint32_t cnt = r.u32();
            r.o += 64;
            for (uint32_t i = 0; i < cnt && r.ok; i++)
                if (!child()) return false;
            break;
        }
        case 6:
            r.u32();
            n.geoset = (int)r.u32();
            n.mattable = (int)r.u32();
            break;
        case 8: {
            if (!r.need(0x58)) return false;
            uint32_t cnt = rd<uint32_t>(r.p + r.o + 0x54);
            r.o += 0x58 + cnt * 12;
            break;
        }
        case 9: {
            if (!r.need(6)) return false;
            uint8_t has = r.p[r.o];
            uint16_t cnt = rd<uint16_t>(r.p + r.o + 2);
            r.o += 6;
            if (has) r.o += cnt * 16;
            for (uint32_t i = 0; i < cnt && r.ok; i++)
                if (!child()) return false;
            break;
        }
        case 10: {
            if (!r.need(0x20)) return false;
            uint8_t has = r.p[r.o];
            r.o += 0x20;
            if (has && !child()) return false;
            break;
        }
        default:
            fprintf(stderr, "FSO: unknown node type %d\n", n.type);
            return false;
    }
    return r.ok;
}

static void parseGeosets(const uint8_t* p, size_t n, FSO& fso) {
    size_t o = 0;
    while (o + 0x60 <= n) {
        size_t g = o + 4;
        uint16_t nmesh = rd<uint16_t>(p + g + 4);
        float lod[16];
        memcpy(lod, p + g + 0x1c, 64);
        size_t q = g + 0x5c;
        Geoset gs;
        for (int mi = 0; mi < nmesh; mi++) {
            uint32_t h[15];
            memcpy(h, p + q, 60);
            uint32_t nv = h[2], nt = h[8], nx = h[11];
            q += 0x3c;
            Mesh m;
            m.lodDist = mi < 16 ? lod[mi] : 0;
            m.pos.resize(nv);
            memcpy(m.pos.data(), p + q, nv * 12); q += nv * 12;
            if (h[4]) { m.nrm.resize(nv); memcpy(m.nrm.data(), p + q, nv * 12); q += nv * 12; }
            if (h[5]) { m.col.resize(nv); memcpy(m.col.data(), p + q, nv * 4); q += nv * 4; }
            if (h[6]) { m.uv0.resize(nv * 2); memcpy(m.uv0.data(), p + q, nv * 8); q += nv * 8; }
            if (h[7]) { m.uv1.resize(nv * 2); memcpy(m.uv1.data(), p + q, nv * 8); q += nv * 8; }
            m.idx.resize(nt * 3);
            memcpy(m.idx.data(), p + q, nt * 6); q += nt * 6;
            if (nt) {
                m.faceMat.resize(nt);
                for (uint32_t t = 0; t < nt; t++) m.faceMat[t] = rd<uint16_t>(p + q + t * 8);
                q += nt * 8;
            }
            m.extraCount = nx;
            q += nx * 8;
            gs.lods.push_back(std::move(m));
        }
        fso.geosets.push_back(std::move(gs));
        o = q;
    }
}

static void parseMattables(const uint8_t* p, size_t n, FSO& fso) {
    Reader r{p, n};
    while (r.o + 0x10 <= n) {
        uint16_t cnt = rd<uint16_t>(p + r.o + 6);
        r.o += 0x10;
        std::vector<std::string> names;
        for (int i = 0; i < cnt; i++) {
            names.push_back(r.cstr());
            r.align4();
        }
        fso.mattables.push_back(std::move(names));
    }
}

static void parseMaterials(const uint8_t* p, size_t n, FSO& fso) {
    Reader r{p, n};
    while (r.o + 0xb4 <= n) {
        Material m;
        const uint8_t* h = p + r.o;
        memcpy(m.ambient, h + 8, 16);
        memcpy(m.diffuse, h + 24, 16);
        uint32_t ntex = rd<uint32_t>(h + 0x50);
        r.o += 0xb4;
        m.name = r.cstr();
        r.align4();
        for (uint32_t i = 0; i < ntex && r.ok; i++) {
            m.textures.push_back(baseName(r.cstr()));
            r.align4();
            m.texFlags.push_back(r.u32());
        }
        fso.materials.push_back(std::move(m));
    }
}

bool loadFSO(const std::string& path, FSO& out) {
    Bytes d;
    if (!readFile(path, d)) return false;
    static const char magic[] = "FSO_Database";
    if (d.size() < 13 + 16) return false;
    if (memcmp(d.data(), magic, 13) != 0) xorDecrypt(d.data(), 13);
    if (memcmp(d.data(), magic, 13) != 0) return false;
    uint32_t sz[4];
    memcpy(sz, &d[d.size() - 16], 16);
    size_t o = 13;
    if (o + sz[0] + sz[1] + sz[2] + sz[3] > d.size()) return false;
    const uint8_t* A = &d[o]; o += sz[0];
    const uint8_t* B = &d[o]; o += sz[1];
    const uint8_t* C = &d[o]; o += sz[2];
    const uint8_t* D = &d[o];
    parseMaterials(D, sz[3], out);
    parseMattables(C, sz[2], out);
    parseGeosets(B, sz[1], out);
    Reader r{A, sz[0]};
    return parseNode(r, out.root, out);
}

// ---------------------------------------------------------------- FST

bool loadFST(const std::string& path, FST& out) {
    Bytes d;
    if (!readFile(path, d)) return false;
    if (d.size() < 20 || memcmp(d.data(), "FST_Terrain", 12) != 0) return false;
    uint32_t nv = rd<uint32_t>(&d[12]), nt = rd<uint32_t>(&d[16]);
    size_t o = 20;
    if (o + nv * 12 + nt * 60 > d.size()) return false;
    out.verts.resize(nv);
    memcpy(out.verts.data(), &d[o], nv * 12);
    o += nv * 12;
    out.tris.resize(nt);
    for (uint32_t i = 0; i < nt; i++) memcpy(&out.tris[i], &d[o + i * 60], 60);
    return true;
}

// ---------------------------------------------------------------- weather

static const size_t kWeatherRec = 2424;

static bool weatherData(Bytes& d) {
    if (!readFile("weather.bin", d)) return false;
    xorDecrypt(d.data(), d.size());
    return true;
}

static std::string fixedStr(const uint8_t* p, size_t n) { return std::string((const char*)p, strnlen((const char*)p, n)); }

std::vector<std::string> weatherPresets() {
    std::vector<std::string> r;
    Bytes d;
    if (!weatherData(d)) return r;
    for (size_t o = 0; o + kWeatherRec <= d.size(); o += kWeatherRec) r.push_back(fixedStr(&d[o], 64));
    return r;
}

bool loadWeather(const std::string& preset, Weather& w) {
    Bytes d;
    if (!weatherData(d)) return false;
    for (size_t o = 0; o + kWeatherRec <= d.size(); o += kWeatherRec) {
        const uint8_t* r = &d[o];
        if (lower(fixedStr(r, 64)) != lower(preset)) continue;
        w.name = fixedStr(r, 64);
        w.fog = r[64] != 0;
        memcpy(w.fogColor, r + 68, 12);
        w.fogStart = rd<float>(r + 84);
        w.fogEnd = rd<float>(r + 88);
        memcpy(w.light, r + 140, 12);
        for (int i = 0; i < 8; i++) {
            std::string from = fixedStr(r + 164 + i * 64, 64), to = fixedStr(r + 676 + i * 64, 64);
            if (!from.empty() && !to.empty()) w.replace.push_back({lower(from), lower(to)});
        }
        return true;
    }
    return false;
}

}  // namespace bsr
