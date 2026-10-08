# Big Scale Racing — WebAssembly port

A browser reimplementation of *Big Scale Racing* (BumbleBeast, 2002) that runs on the **original game data**.
The engine is new C++ compiled to WebAssembly (WebGL 2). It decodes the game's own encrypted/packed formats
at runtime, so the data files are served unmodified. See [FORMATS.md](FORMATS.md) for the reverse-engineered formats and
[docs/TECHNICAL.md](docs/TECHNICAL.md) for the full technical documentation.

What works: all 6 tracks, all 11 car classes with their 12 skins each, AI opponents on the original racing lines,
checkpoints/laps/positions, the original sky/fog weather presets, engine/skid/impact sounds, music, keyboard,
gamepad and touch controls, two-player split-screen, and online races for up to 8 players (new; the original
only had local multiplayer).

Not reproduced: the original `bamms.dll` multibody physics (replaced by a raycast-suspension car model tuned
to the original top speeds), menus/career mode, rain particles, replays.

## Build

Requirements: Emscripten (`em++`), Python 3. The game data comes from your own copy of the CD.

```sh
# 1. Extract the game data from the CD image (InstallShield 5 cabinet inside the ISO)
bsdtar -xf BIGSCALERACING01.iso -C iso/            # or mount the .cue/.bin
unshield -d game x iso/data1.cab                    # https://github.com/twogood/unshield

# 2. Copy the files the web build needs into web/data (unmodified, paths lowercased) + manifest.json
python3 tools/pack.py game/Program_Executable_Files_Group/Data web

# 3. Compile the engine to web/bsr.js + web/bsr.wasm
./build.sh

# 4. Serve web/ over HTTP and open http://localhost:8000
python3 tools/devserver.py 8000
```

`web/data` is about 140 MB in total; a race loads only one track's files (9–18 MB) plus the shared cars and sounds.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Throttle / brake & reverse | ↑ / ↓ or W / S | RT / LT (or A / X) |
| Steer | ← → or A D | left stick |
| Handbrake | Space | B |
| Camera (chase / far / bumper) | C | |
| Reset car onto the track | R | |
| Pause | P or Esc | |
| Debug lines (checkpoints, AI line) | F2 | |

Split screen: player 1 uses WASD, Space (handbrake), C (camera) and R (reset); player 2 uses the arrows, Right Shift,
`.` and Backspace. A single gamepad goes to player 2; with two or more, pad 1 is player 1 and pad 2 player 2.

## Online multiplayer

Pick **Online** in the menu, enter a name and **Create room**. Share the 4-letter code or **Copy invite link**;
friends pick **Join** with the code. The host chooses track, class, weather, AI cars and laps; everyone chooses
their own skin. Up to 8 players per room.

The rooms run in `tools/devserver.py` (WebSocket at `/ws`, Python standard library only). To play with others,
serve on all interfaces and open the port (or run it on a server, behind an HTTPS proxy for `wss://`):

```sh
python3 tools/devserver.py 8000 --host 0.0.0.0
```

A page served from elsewhere (e.g. static hosting) can point at a relay with `?server=wss://host/ws`.

## Layout

```
src/formats.*   decryption and parsers: .fsp textures, .tga, .fso scenes, .fst collision, weather.bin
src/render.*    WebGL2/GLES3 renderer: lightmaps, env maps, alpha, fog
src/physics.*   collision grid over the .fst mesh, raycast-suspension car with collision spheres
src/race.*      meta splines (grid, checkpoints, AI lines), AI driver
src/game.*      race flow, cameras/split-screen, drawing (platform independent)
src/netsync.*   online play: car state snapshots and interpolation of remote cars
src/main.cpp    browser glue: input, canvas, JS callbacks
web/            index.html, game.js (menu, loader, HUD, audio), style.css
tools/pack.py   data packer; tools/bsrfmt.py Python format library
tools/harness/  native headless test (EGL + Mesa): races with all cars on autopilot, writes screenshots
tools/nodetest.js  browser-layer integration test in Node (stub DOM/WebGL/WebAudio, real wasm)
```

## Tests

```sh
tools/harness/build.sh
OPP=7 LAPS=3 tools/harness/harness web/data baanbreker pro_std /tmp/shots 150 5 20   # race + screenshots at 5s, 20s
node tools/nodetest.js mach pro_std 50                                                # menu → load → race → finish
```
