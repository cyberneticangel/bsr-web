# Big Scale Racing data formats

Reverse-engineered from the retail CD (BumbleBeast, 2002) and `Big Scale Racing.exe` (decompiled with Ghidra).
All integers are little-endian. World space is right-handed, Z up, metres; cars are ~1:5 scale RC models.

## Disc layout

`BIGSCALERACING.bin/.cue` is a single MODE1/2352 track. The ISO holds an InstallShield 5 installer; the game
itself is in `data1.cab` (extract with [unshield](https://github.com/twogood/unshield): `unshield -d out x data1.cab`).

| Path | Contents |
|---|---|
| `Big Scale Racing.exe` | game (Direct3D 7 via DirectDraw, loaded dynamically) |
| `bamms.dll` | vehicle multibody physics ("bamms" model files in `Data/Cars/*/bamms_*.bin`) |
| `Data/Tracks` | track scenery (`.fso`), collision (`.fst`), metadata (`_meta.fso`), cameras, weather |
| `Data/Cars` | car models per class, skin tables (`.fsm`), class parameters |
| `Data/Maps_high`, `Maps_low` | textures (`.fsp`), per-track subfolders |
| `Data/Maps_cars` | car skins, plain uncompressed 32-bit `.tga` |
| `Data/Audio`, `Data/Music` | `.fsw` (WAV), `.fs3` (MP3) |

## Encryption

Function `0x457780` XORs every `fread()` buffer with a 16-byte key stored at `0x4b52d8`:

```
47 c3 f5 12 38 e9 b5 91 25 63 06 d9 aa 6f 3a 73
```

The key index restarts at 0 for each read call, so a file read in pieces looks like it uses a shifted key
(e.g. `Materials.bin`). Encrypted: `.fs3`, WAV sample data, texture payloads, most `.bin` files.

## `.fsw` audio

Standard RIFF/WAVE with a plaintext header; the bytes of the `data` chunk are XORed with the key (index 0 at
the start of the chunk data). 22/44 kHz, 16-bit PCM.

## `.fs3` music

Whole file XORed with the key → MP3 (LAME 3.92). `Data/Music.ini` maps game states to tracks.

## `.fsp` textures

```
u32 width, height, format, mipCount(=1), dataSize
[format 1 only: 256 x BGRA palette, 1024 bytes, plaintext]
u8  data[dataSize]          // XORed with the key, then TGA-style RLE
```

RLE packets: header byte `c`; `n = (c & 0x7f) + 1`. If `c & 0x80` one pixel follows, repeated `n` times;
otherwise `n` literal pixels follow. Rows are stored bottom-up.

| format | pixel | used in |
|---|---|---|
| 1 | 8-bit palette index | shadow / spray maps |
| 2 | RGB565 | `Maps_low` opaque |
| 4 | ARGB1555 | rare |
| 5 | ARGB4444 | `Maps_low` alpha |
| 7 | BGR888 | `Maps_high` opaque |
| 11 | BGRA8888 | `Maps_high` alpha |

UVs are 3ds Max style (v = 0 at the bottom), which matches bottom-up rows uploaded unchanged.

## `.fso` scene database (`FSO_Database`)

```
char magic[13] = "FSO_Database\0"      // occasionally XORed
u8   nodes[sizeA]; u8 geosets[sizeB]; u8 mattables[sizeC]; u8 materials[sizeD]
u32  sizeA, sizeB, sizeC, sizeD         // last 16 bytes of the file
```

**Nodes** (recursive, root first). Common 0x3C-byte header: `u32 type`, name at byte 9 (to 0x2C), bounding
sphere `float[4]` at 0x2C. Then by type:

| type | meaning | payload |
|---|---|---|
| 1 | group (1 child) | `u8 hasChild` |
| 2 | transform | `u8 flag, u8 hasChild, pad2, float m[16]` (D3D row-major) |
| 3 | list | `u32 count` + children |
| 4 | light | `u8 hasChild, pad3, float[16]`: [1..3] direction, [4..7] diffuse, [8..11] ambient |
| 5 | switch / LOD | `u32 count, float ranges[16]` + children |
| 6 | mesh instance | `i32 ?, i32 geoset, i32 mattable` |
| 8 | (unused by tracks) | 0x58 bytes + `count*12` |
| 9 | (unused by tracks) | `u8 has, pad, u16 n`, `n*16` if has, + n children |
| 10 | (unused by tracks) | 0x20 bytes |

**Geosets**: `u32 prefix`, then header: `u16 meshCount` at +4, `float centre[3]` at +8, `float lodDist[16]` at
+0x1C, meshes at +0x5C. Each mesh is a LOD level (0 = most detailed):

```
u32 hdr[15]: [2]=vertexCount [4]=hasNormals [5]=hasColors [6]=hasUV0 [7]=hasUV1 [8]=triCount [11]=extraCount
float pos[n][3]; float nrm[n][3]?; u32 bgra[n]?; float uv0[n][2]?; float uv1[n][2]?
u16 idx[triCount][3]          // CCW front faces
u8  face[triCount][8]          // u16 material slot (index into the node's mattable), ...
u8  extra[extraCount][8]
```

**Mattables**: `u8[4], u8 flag, pad, u16 count, u8[8]`, then `count` material names (NUL-terminated, each
padded to 4 bytes).

**Materials**: 0xB4-byte struct (`float ambient[4]` at 0x08, `float diffuse[4]` at 0x18, `u32 texCount` at
0x50), name, then per texture: path string (4-aligned) + `u32 flags`. Paths are the artists' Windows paths; only
the base name matters. A second texture named `XT_shadow*` is a lightmap on UV1, `FX_environment*` an
environment map.

Car models: `m_body`, `m_whl_lf/rf/lr/rr` (wheels modelled in place), `m_shadow`, `m_coll_sphere_NN`
(collision spheres). Tracks: `g_trackgeo`, `g_publiek` (crowd), `g_reflection` (mirrored copies for wet weather),
`x_dome` (sky dome using placeholder texture `do_domereplace`).

## `_meta.fso` track metadata

An FSO whose meshes are 3ds Max splines: vertices come in `[knot, inHandle, outHandle]` triples (cubic
Bezier). Names:

* `m_meta_gridposNN` – 2-knot line: start position and facing direction (24 slots)
* `m_meta_checkpointNN` – gate line across the track; `01` is start/finish; names ending in `P` are pit-lane only
* `m_meta_aii01..06` – closed AI racing lines (06 is an alternate/pit line)
* `m_meta_aisl01`, `m_meta_aisr01` – left/right track edges
* `m_meta_pitposNN`, `podiumposNN`, `prizeposNN`

## `.fst` collision (`FST_Terrain`)

```
char magic[12] = "FST_Terrain\0"
u32 vertexCount, triCount
float verts[vertexCount][3]
struct { float f0; u32 v[3]; u32 surface; float normal[3]; float d; float bmin[3], bmax[3]; } tris[triCount]  // 0x3C
quadtree nodes (0x30-byte headers + triangle index lists)
```

## `weather.bin`

XORed; 47 records of 2424 bytes: `char name[64]`, `u8 fogEnable` at 64, `float fogColor[3]` at 68,
`float fogStart, fogEnd` at 84, `float light[3]` at 140, 8 texture-swap pairs (`char from[64]` at 164 + 64·i,
`char to[64]` at 676 + 64·i; e.g. `do_domereplace → do_T_512`), environment map name at 2348.

## Other

* `Data/hud.ini` – speedometer scale per class (km/h); used as top speed.
* `Data/Languages/English.bin` – XORed string table (`u32 id`, key, text).
* `Cars/class_*.bin` – XORed (read in several chunks); per-class sound/HUD/effect asset names.
* `Cars/<track>/bamms_<class>_{mid,high}.bin` – multibody model definitions for `bamms.dll` (not used by this port).
