"""Big Scale Racing (BumbleBeast, 2002) asset format helpers."""
import struct

KEY = bytes.fromhex('47c3f51238e9b591256306d9aa6f3a73')  # exe @ 0x4b52d8


def xor(data, start=0):
    """Game XORs each fread() buffer independently with KEY (index restarts per buffer)."""
    k = (KEY * (len(data) // 16 + 2))[start % 16:start % 16 + len(data)]
    return bytes(a ^ b for a, b in zip(data, k)) if len(data) < 4096 else _xor_fast(data, k)


def _xor_fast(data, k):
    a = int.from_bytes(data, 'little'); b = int.from_bytes(k, 'little')
    return (a ^ b).to_bytes(len(data), 'little')


FSP_BPP = {1: 1, 2: 2, 4: 2, 5: 2, 7: 3, 11: 4}


def rle_decode(src, npix, bpp):
    out = bytearray(); i = 0
    while len(out) < npix * bpp and i < len(src):
        c = src[i]; i += 1; n = (c & 0x7f) + 1
        if c & 0x80:
            out += src[i:i + bpp] * n; i += bpp
        else:
            out += src[i:i + n * bpp]; i += n * bpp
    return bytes(out), i


def read_fsp(data):
    w, h, fmt, mips, size = struct.unpack_from('<5I', data)
    off, pal = 20, None
    if fmt == 1:  # 8-bit paletted: plaintext 256xBGRA palette precedes the data
        pal = data[20:1044]; off = 1044
    payload = xor(data[off:off + size])
    bpp = FSP_BPP[fmt]
    pix, used = rle_decode(payload, w * h, bpp)
    return dict(w=w, h=h, fmt=fmt, mips=mips, bpp=bpp, pixels=pix, used=used, size=size, palette=pal)


def fsp_to_rgba(t):
    """Convert decoded fsp to top-down RGBA8 bytes."""
    w, h, fmt, p = t['w'], t['h'], t['fmt'], t['pixels']
    out = bytearray(w * h * 4)
    if fmt == 1:
        pal = t['palette']
        for i, v in enumerate(p):
            b, g, r, a = pal[v * 4:v * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    elif fmt in (7,):
        for i in range(w * h):
            b, g, r = p[i * 3:i * 3 + 3]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    elif fmt in (11,):
        for i in range(w * h):
            b, g, r, a = p[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    elif fmt in (2, 4, 5):
        for i in range(w * h):
            v = p[i * 2] | p[i * 2 + 1] << 8
            if fmt == 2:  # RGB565
                r, g, b, a = (v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255
            elif fmt == 5:  # ARGB4444
                a, r, g, b = (v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17
            else:  # ARGB1555
                a, r, g, b = (v >> 15) * 255, (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return bytes(out)


def write_png(path, w, h, rgba):
    import zlib
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def _cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin1'), e + 1


def _align4(o):
    return (o + 3) & ~3


def read_fso(data):
    """FSO_Database: magic, 4 sections (nodes, geosets, mattables, materials); sizes in last 16 bytes."""
    magic = b'FSO_Database\0'
    if not data.startswith(magic):
        data = xor(data[:13]) + data[13:]
        assert data.startswith(magic), 'bad FSO magic'
    sa, sb, sc, sd = struct.unpack_from('<4I', data, len(data) - 16)
    o = 13
    A = data[o:o + sa]; o += sa
    B = data[o:o + sb]; o += sb
    C = data[o:o + sc]; o += sc
    D = data[o:o + sd]; o += sd
    return dict(nodes=_parse_nodes(A), geosets=_parse_geosets(B), mattables=_parse_mattables(C),
                materials=_parse_materials(D), sizes=(sa, sb, sc, sd))


def _parse_materials(D):
    mats, o = [], 0
    while o < len(D):
        hdr = D[o:o + 0xb4]
        ntex = struct.unpack_from('<I', hdr, 0x50)[0]
        name, o2 = _cstr(D, o + 0xb4)
        o = _align4(o2)
        texs = []
        for _ in range(ntex):
            tn, o2 = _cstr(D, o); o = _align4(o2)
            flags = struct.unpack_from('<I', D, o)[0]; o += 4
            texs.append((tn, flags))
        mats.append(dict(name=name, textures=texs, raw=hdr))
    return mats


def _parse_mattables(C):
    tabs, o = [], 0
    while o < len(C):
        flag = C[o + 4]; n = struct.unpack_from('<H', C, o + 6)[0]
        p = o + 0x10; names = []
        for _ in range(n):
            s, p = _cstr(C, p); p = _align4(p); names.append(s)
        tabs.append(dict(flag=flag, materials=names, hdr=C[o:o + 0x10]))
        o = p
    return tabs


def _parse_geosets(B):
    gs, o = [], 0
    while o < len(B):
        pre = struct.unpack_from('<I', B, o)[0]
        g = o + 4
        nmesh = struct.unpack_from('<H', B, g + 4)[0]
        center = struct.unpack_from('<3f', B, g + 8)
        radius_like = struct.unpack_from('<f', B, g + 0x14)[0]
        slots = struct.unpack_from('<16i', B, g + 0x1c)
        p = g + 0x5c; meshes = []
        for mi in range(nmesh):
            h = struct.unpack_from('<15I', B, p)
            nv, nt, nx = h[2], h[8], h[11]
            q = p + 0x3c
            m = dict(hdr=h, slot=slots[mi])
            m['pos'] = B[q:q + nv * 12]; q += nv * 12
            if h[4]: m['nrm'] = B[q:q + nv * 12]; q += nv * 12
            if h[5]: m['col'] = B[q:q + nv * 4]; q += nv * 4
            for k, key in ((6, 'uv0'), (7, 'uv1')):
                if h[k]: m[key] = B[q:q + nv * 8]; q += nv * 8
            m['idx'] = B[q:q + nt * 6]; q += nt * 6
            if nt: m['face'] = B[q:q + nt * 8]; q += nt * 8
            if nx: m['extra'] = B[q:q + nx * 8]; q += nx * 8
            m['nv'], m['nt'] = nv, nt
            meshes.append(m); p = q
        gs.append(dict(pre=pre, center=center, r=radius_like, meshes=meshes, flag=B[g + 0x18]))
        o = p
    return gs


NODE_TYPES = {1: 'group', 2: 'transform', 3: 'list', 4: 'type4', 5: 'switch', 6: 'mesh', 8: 'type8', 9: 'type9', 10: 'type10'}


def _parse_nodes(A):
    pos = [0]

    def node():
        o = pos[0]
        h = struct.unpack_from('<15I', A, o)
        typ = h[0]
        name = A[o + 9:o + 0x2c].split(b'\0')[0].decode('latin1')
        bs = struct.unpack_from('<4f', A, o + 0x2c)
        o += 0x3c
        n = dict(type=typ, kind=NODE_TYPES.get(typ, '?'), name=name, h1=h[1], b8=A[o - 0x3c + 8], bsphere=bs, children=[])
        if typ == 1:
            has = A[o]; pos[0] = o + 1
            if has: n['children'].append(node())
        elif typ == 2:
            n['flag'] = A[o]; has = A[o + 1]
            n['matrix'] = struct.unpack_from('<16f', A, o + 4); pos[0] = o + 0x44
            if has: n['children'].append(node())
        elif typ == 4:
            has = A[o]; n['data'] = struct.unpack_from('<16f', A, o + 4); pos[0] = o + 0x44
            if has: n['children'].append(node())
        elif typ == 3:
            cnt = struct.unpack_from('<I', A, o)[0]; pos[0] = o + 4
            for _ in range(cnt): n['children'].append(node())
        elif typ == 5:
            cnt = struct.unpack_from('<I', A, o)[0]
            n['ranges'] = struct.unpack_from('<16f', A, o + 4); pos[0] = o + 0x44
            for _ in range(cnt): n['children'].append(node())
        elif typ == 6:
            n['u0'], n['geoset'], n['mattable'] = struct.unpack_from('<3i', A, o); pos[0] = o + 12
        elif typ == 8:
            n['raw'] = A[o:o + 0x58]
            cnt = struct.unpack_from('<I', A, o + 0x54)[0]
            pos[0] = o + 0x58 + cnt * 12
        elif typ == 9:
            has = A[o]; cnt = struct.unpack_from('<H', A, o + 2)[0]; o += 6
            if has: n['raw'] = A[o:o + cnt * 16]; o += cnt * 16
            pos[0] = o
            for _ in range(cnt): n['children'].append(node())
        elif typ == 10:
            has = A[o]; n['data'] = struct.unpack_from('<7f', A, o + 4); pos[0] = o + 0x20
            if has: n['children'].append(node())
        else:
            raise ValueError('unknown node type %d at %d' % (typ, o - 0x3c))
        return n
    root = node()
    return root
