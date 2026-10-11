#!/usr/bin/env python3
"""Copy the original Big Scale Racing data files needed by the web build into web/data.

Files are copied unmodified (still XOR-encrypted / RLE-packed); the WASM engine and the
JS audio layer decode them at runtime. Paths are lowercased because the original game ran
on a case-insensitive filesystem.

usage: pack.py <game Data dir> <web dir>
"""
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsrfmt  # noqa: E402

TRACKS = ['baanbreker', 'de_sluis', 'evmc', 'mach', 'real80', 'velodrome']
TRACK_NAMES = {'baanbreker': 'Baanbreker', 'de_sluis': 'De Sluis', 'evmc': 'EVMC',
               'mach': 'Mach', 'real80': 'Real 80', 'velodrome': 'Velodrome'}
CLASSES = ['junior_std', 'junior_hop', 'trained_std', 'trained_hop', 'skilled_std', 'skilled_hop',
           'expert_std', 'expert_hop', 'pro_std', 'pro_hop', 'Monster']
SOUNDS = ['FX_Engine_st_22_ZENOAH_A', 'FX_Engine_st_22_serpent_A', 'FX_tire_slip_A', 'FX_auto_botst_b',
          'FX_auto_botst_c', 'AU_FX_botst_alg_A', 'FX_TireWall_A', 'FX_horn_finish_22_mo_A', 'FX_false_start',
          'FX_env_crowd_gen_A_22', 'menu_action', 'menu_updown', 'menu_leftright', 'FX_RCdrivetrain_A_44', 'FX_WARNING',
          'FX_tire_skid_K', 'FX_env_crowd_applause_22', 'FX_weather_rain_steady_A', 'FX_weather_rain_heavy',
          'FX_weather_rain_light', 'FX_weather_thunder_close_A', 'FX_weather_thunder_far_A',
          'FX_weather_thunder_very_far_A', 'FX_weather_thunder_nearby']
SOUND_PREFIXES = ['FX_Comment_']  # every announcer line (three voices: IJB, MW, PS)
WEATHER = {'Sunny': 'sunny_bluesky_summer', 'Summer haze': 'sunny_neut_clouds_summer', 'Sunset': 'sunset_orange',
           'Cloudy': 'cloudy_partly_NW_NR_NT', 'Foggy': 'cloudy_gray_foggy_NW_NR_NT', 'Wet': 'wet_neutral_A'}
# Data/Music.ini: music per game state, and three genres of on-track music. Every on-track song exists as
# .fs3 (stereo MP3) and .fsw (mono 11 kHz WAV, for "localized" music played from the trackside speakers).
MUSIC_STATES = {'menu': 'music_bsr_take3a', 'finish': 'music_bsr_take5', 'podium': '61 Track61',
                'nopodium': '62 Track62'}
MUSIC_GENRES = {'surf': r'bsr_track\d+', 'rock': r'bsr_edge_track\d+', 'dance': r'bsr_night_track\d+'}


def main(data, web):
    out = os.path.join(web, 'data')
    index = {}
    for root, _, files in os.walk(data):
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, data).replace(os.sep, '/')
            index.setdefault(rel.lower(), full)
    by_base = {}
    for rel in index:
        base = rel.rsplit('/', 1)[-1].rsplit('.', 1)[0]
        by_base.setdefault(base, []).append(rel)

    def tex_files(texname, track):
        base = texname.replace('\\', '/').split('/')[-1].lower().rsplit('.', 1)[0]
        cands = by_base.get(base, [])
        pref = ['maps_high/%s/' % track, 'maps_high/', 'maps_cars/']
        for p in pref:
            for c in cands:
                if c.startswith(p) and c.count('/') == p.count('/'):
                    return [c]
        return []

    def fso_textures(rel, track):
        r = bsrfmt.read_fso(open(index[rel], 'rb').read())
        res = set()
        for m in r['materials']:
            for t, _ in m['textures']:
                res.update(tex_files(t, track))
        return res

    used = set()
    manifest = {'tracks': {}, 'classes': {}, 'common': [], 'sounds': {}, 'music': [], 'trackNames': TRACK_NAMES}
    common = set()
    for c in CLASSES:
        rel = 'cars/car_%s.fso' % c.lower()
        cls_files = {rel} | fso_textures(rel, '')
        common |= cls_files
        skins = []
        for i in range(1, 13):
            fsm = 'cars/mats_%s_%02d.fsm' % (c.lower(), i)
            if fsm not in index:
                continue
            raw = open(index[fsm], 'rb').read()
            for t in re.findall(rb'([A-Za-z0-9_\-]+)\.tga', raw):
                t = t.decode().lower()
                fl = tex_files(t, '')
                if fl and fl[0].startswith('maps_cars/'):
                    skins.append(t)
                    cls_files.update(fl)
                    break
        common |= cls_files
        # files: everything needed to draw this class (the menu loads them for the skin previews)
        manifest['classes'][c] = {'skins': skins, 'files': sorted(cls_files)}
    names = list(SOUNDS)
    for rel in sorted(index):
        base = os.path.basename(index[rel]).rsplit('.', 1)[0]
        if rel.startswith('audio/') and rel.endswith('.fsw') and any(base.startswith(p) for p in SOUND_PREFIXES):
            names.append(base)
    for s in names:
        cands = [r for r in by_base.get(s.lower(), []) if r.startswith('audio/') and r.endswith('.fsw')]
        if cands:
            manifest['sounds'][s] = cands[0]
            used.add(cands[0])
    music = {}
    for state, base in MUSIC_STATES.items():
        rel = 'music/%s.fs3' % base.lower()
        if rel in index:
            music[state] = rel
            used.add(rel)
    for genre, pat in MUSIC_GENRES.items():
        songs = []
        for rel in sorted(index):
            m = re.fullmatch(r'music/(%s)\.fs3' % pat, rel)
            wav = m and 'music/%s.fsw' % m.group(1)
            if m and wav in index:
                songs.append({'mp3': rel, 'wav': wav})
                used.update((rel, wav))
        music[genre] = songs
    manifest['music'] = music
    for t in TRACKS:
        files = set()
        for suf in ('_high.fso', '_high.fst', '_meta.fso', '.bin'):
            rel = 'tracks/%s%s' % (t, suf)
            if rel in index:
                files.add(rel)
        files |= fso_textures('tracks/%s_high.fso' % t, t)
        manifest['tracks'][t] = sorted(files)
        used |= files
    # hud.ini: per class/track speedometer scale (km/h) -> used as top speed.
    hud = open(index['hud.ini'], encoding='latin1').read()
    for m in re.finditer(r'(\w+)\s*\{([^}]*)\}', hud):
        nums = re.findall(r'\w+\s+(\d+)\s+\d+', m.group(2))
        for c in manifest['classes']:
            if c.lower() == m.group(1).lower() and nums:
                manifest['classes'][c]['topSpeed'] = int(nums[0])
    manifest['classes']['Monster'].setdefault('topSpeed', 50)
    # Weather presets: the sky dome / env textures they swap in.
    common.add('weather.bin')
    wx = bsrfmt.xor(open(index['weather.bin'], 'rb').read())
    for o in range(0, len(wx) - 2423, 2424):
        for i in range(8):
            to = wx[o + 676 + i * 64:o + 740 + i * 64].split(b'\0')[0].decode('latin1')
            if to:
                common.update(tex_files(to, ''))
    manifest['weather'] = WEATHER
    manifest['common'] = sorted(common)
    used |= common
    total = 0
    for rel in sorted(used):
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(index[rel]):
            shutil.copyfile(index[rel], dst)
        total += os.path.getsize(index[rel])
    with open(os.path.join(out, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print('packed %d files, %.1f MB' % (len(used), total / 1e6))
    for t in TRACKS:
        sz = sum(os.path.getsize(index[r]) for r in manifest['tracks'][t])
        print('  %-11s %4d files %6.1f MB' % (t, len(manifest['tracks'][t]), sz / 1e6))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
