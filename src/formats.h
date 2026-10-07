// Loaders for the original Big Scale Racing (BumbleBeast, 2002) data formats.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bsr {

using Bytes = std::vector<uint8_t>;

extern std::string gDataRoot;  // "/data/" in the browser (MEMFS)
// Reads <gDataRoot>/<lowercased path>. Returns false if missing.
bool readFile(const std::string& path, Bytes& out);
std::string lower(std::string s);
std::string baseName(const std::string& path);  // lowercased, without dir and extension

// Every fread() buffer in the game is XORed with a 16-byte key (exe @ 0x4b52d8); index restarts per buffer.
void xorDecrypt(uint8_t* p, size_t n);

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // rows bottom-up, as stored by the game (TGA convention)
    bool hasAlpha = false;
};
bool decodeFSP(const Bytes& data, Image& img);
bool decodeTGA(const Bytes& data, Image& img);
bool loadTexture(const std::string& name, const std::string& trackDir, Image& img);

struct Vec3 { float x, y, z; };

struct Material {
    std::string name;
    float ambient[4] = {1, 1, 1, 1};
    float diffuse[4] = {1, 1, 1, 1};
    std::vector<std::string> textures;  // base names
    std::vector<uint32_t> texFlags;
};

struct Mesh {  // one LOD of a geoset
    float lodDist = 0;
    std::vector<Vec3> pos, nrm;
    std::vector<uint32_t> col;  // BGRA
    std::vector<float> uv0, uv1;
    std::vector<uint16_t> idx;      // triangles
    std::vector<uint16_t> faceMat;  // per-triangle index into mattable
    uint32_t extraCount = 0;
};

struct Geoset { std::vector<Mesh> lods; };

struct Node {
    int type = 0;
    std::string name;
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int geoset = -1, mattable = -1;
    std::vector<Node> children;
};

struct Light { Vec3 dir; float diffuse[4], ambient[4]; bool valid = false; };

struct FSO {
    Node root;
    Light sun;
    std::vector<Geoset> geosets;
    std::vector<std::vector<std::string>> mattables;  // material names
    std::vector<Material> materials;
    const Material* findMaterial(const std::string& n) const;
};
bool loadFSO(const std::string& path, FSO& out);

struct CollTri {  // 0x3c bytes on disk
    float f0;
    uint32_t v[3];
    uint32_t surface;
    Vec3 n;
    float d;
    Vec3 bmin, bmax;
};
struct FST {
    std::vector<Vec3> verts;
    std::vector<CollTri> tris;
};
bool loadFST(const std::string& path, FST& out);

// weather.bin: 2424-byte XOR-encrypted records (sky/fog/light presets).
struct Weather {
    std::string name;
    bool fog = false;
    float fogColor[3] = {0.7f, 0.75f, 0.8f};
    float fogStart = 0, fogEnd = 0;
    float light[3] = {1, 1, 1};
    std::vector<std::pair<std::string, std::string>> replace;  // texture swaps, e.g. dome placeholder -> sky
};
bool loadWeather(const std::string& preset, Weather& out);
std::vector<std::string> weatherPresets();

}  // namespace bsr
