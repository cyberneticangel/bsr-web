// Big Scale Racing — browser entry point (Emscripten): input, canvas, JS bridge.
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"
#include "netsync.h"

using namespace bsr;

EM_JS(void, js_hud, (int player, const char* json), { if (Module.onHud) Module.onHud(UTF8ToString(json), player); });
EM_JS(void, js_audio, (int player, float rpm, float throttle, float skid, float impact, float speed),
      { if (Module.onAudio) Module.onAudio(rpm, throttle, skid, impact, speed, player); });
EM_JS(void, js_event, (const char* name), { if (Module.onEvent) Module.onEvent(UTF8ToString(name)); });

namespace {

Game game;
double lastT = 0;
float hudTimer = 0;
float steerSmooth[Game::kMaxLocal] = {};
float touchSteer = 0, touchThrottle = 0, touchBrake = 0;
bool online = false;  // no pausing: the other players keep racing

// Single player: arrows or WASD, Space handbrake, C camera, R reset.
// Split-screen: player 1 WASD / Space / C / R, player 2 arrows / Right Shift / . / Backspace.
enum Key { kUp, kDown, kLeft, kRight, kW, kS, kA, kD, kSpace, kShiftR, kC, kR, kP, kF2, kPeriod, kBackspace, kCount };
bool keys[kCount] = {};

int keyIndex(const char* code) {
    static const char* names[kCount] = {"ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "KeyW",   "KeyS",
                                        "KeyA",    "KeyD",      "Space",     "ShiftRight", "KeyC",   "KeyR",
                                        "KeyP",    "F2",        "Period",    "Backspace"};
    for (int i = 0; i < kCount; i++)
        if (!strcmp(code, names[i])) return i;
    return -1;
}

EM_BOOL onKey(int type, const EmscriptenKeyboardEvent* e, void*) {
    int k = keyIndex(e->code);
    if (k < 0) return EM_FALSE;
    bool down = type == EMSCRIPTEN_EVENT_KEYDOWN;
    bool split = game.localPlayers() > 1;
    if (down && !e->repeat) {
        if (k == kC) game.cycleCamera(0);
        if (k == kR) game.resetPlayer(0);
        if (split && k == kPeriod) game.cycleCamera(1);
        if (split && k == kBackspace) game.resetPlayer(1);
        if (k == kF2) game.debugLines = !game.debugLines;
        if (k == kP && !online && (game.state() == Game::Racing || game.state() == Game::Countdown)) {
            game.paused = !game.paused;
            js_event(game.paused ? "paused" : "resumed");
        }
    }
    keys[k] = down;
    return k <= kShiftR || (split && k == kBackspace) ? EM_TRUE : EM_FALSE;  // swallow driving keys so the page doesn't scroll
}

struct Pad { float thr = 0, brk = 0, steer = 0; bool steerActive = false, handbrake = false; };

Pad readPad(const EmscriptenGamepadEvent& gp) {
    Pad p;
    if (gp.numAxes > 0 && std::fabs(gp.axis[0]) > 0.15f) { p.steer = (float)gp.axis[0]; p.steerActive = true; }
    if (gp.numButtons > 7) {
        p.thr = (float)gp.analogButton[7];
        p.brk = (float)gp.analogButton[6];
    }
    if (gp.numButtons > 0 && gp.digitalButton[0]) p.thr = 1;
    if (gp.numButtons > 2 && gp.digitalButton[2]) p.brk = 1;
    if (gp.numButtons > 1 && gp.digitalButton[1]) p.handbrake = true;
    return p;
}

// Fills one Input per local player.
void readInputs(float dt, Input* in, int n) {
    float thr[Game::kMaxLocal] = {}, brk[Game::kMaxLocal] = {}, st[Game::kMaxLocal] = {};
    bool analog[Game::kMaxLocal] = {}, hb[Game::kMaxLocal] = {};
    auto kb = [&](int p, Key up, Key dn, Key l, Key r, Key handbrake) {
        thr[p] = std::fmax(thr[p], keys[up] ? 1.0f : 0.0f);
        brk[p] = std::fmax(brk[p], keys[dn] ? 1.0f : 0.0f);
        st[p] += (keys[r] ? 1.0f : 0.0f) - (keys[l] ? 1.0f : 0.0f);
        hb[p] = hb[p] || keys[handbrake];
    };
    if (n > 1) {
        kb(0, kW, kS, kA, kD, kSpace);
        kb(1, kUp, kDown, kLeft, kRight, kShiftR);
    } else {
        thr[0] = (keys[kUp] || keys[kW]) ? 1.0f : 0.0f;
        brk[0] = (keys[kDown] || keys[kS]) ? 1.0f : 0.0f;
        st[0] = ((keys[kRight] || keys[kD]) ? 1.0f : 0.0f) - ((keys[kLeft] || keys[kA]) ? 1.0f : 0.0f);
        hb[0] = keys[kSpace];
    }
    for (int p = 0; p < n; p++) st[p] = clampf(st[p], -1, 1);
    // Sampling must come first: get_num_gamepads reads the sampled state.
    int np = emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS ? emscripten_get_num_gamepads() : 0;
    Pad pads[8];
    int connected = 0;
    for (int i = 0; i < np && connected < 8; i++) {
        EmscriptenGamepadEvent gp;
        if (emscripten_get_gamepad_status(i, &gp) == EMSCRIPTEN_RESULT_SUCCESS && gp.connected) pads[connected++] = readPad(gp);
    }
    for (int i = 0; i < connected; i++) {
        // Single player: every pad drives. Split-screen: pads go to players in order, except that a lone pad
        // goes to player 2 so that player 1 has the keyboard to themselves.
        int p = n == 1 ? 0 : (connected == 1 ? 1 : std::min(i, n - 1));
        const Pad& pad = pads[i];
        if (pad.steerActive) { st[p] = pad.steer; analog[p] = true; }
        thr[p] = std::fmax(thr[p], pad.thr);
        brk[p] = std::fmax(brk[p], pad.brk);
        hb[p] = hb[p] || pad.handbrake;
    }
    if (std::fabs(touchSteer) > 0.02f) { st[0] = touchSteer; analog[0] = true; }
    thr[0] = std::fmax(thr[0], touchThrottle);
    brk[0] = std::fmax(brk[0], touchBrake);
    for (int p = 0; p < n; p++) {
        float& sm = steerSmooth[p];
        if (analog[p]) {
            sm = st[p];
        } else {  // keyboard: ramp steering like an RC transmitter wheel
            float rate = (st[p] == 0 || st[p] * sm < 0) ? 6.0f : 3.5f;
            sm += clampf(st[p] - sm, -rate * dt, rate * dt);
        }
        in[p].steer = sm;
        in[p].throttle = thr[p];
        in[p].brake = brk[p];
        in[p].handbrake = hb[p];
    }
}

void frame() {
    double now = emscripten_get_now() / 1000.0;
    float dt = (float)std::fmin(0.1, now - lastT);
    lastT = now;
    if (game.state() == Game::Idle) return;
    int w, h;
    emscripten_get_canvas_element_size("#canvas", &w, &h);
    double cw, ch;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    double dpr = emscripten_get_device_pixel_ratio();
    int tw = (int)(cw * dpr), th = (int)(ch * dpr);
    if (tw != w || th != h) {
        emscripten_set_canvas_element_size("#canvas", tw, th);
        w = tw;
        h = th;
    }
    Input in[Game::kMaxLocal];
    int n = std::max(1, std::min(game.localPlayers(), (int)Game::kMaxLocal));
    readInputs(dt, in, n);
    game.setNow(now);
    game.update(dt, in, n);
    game.render(w, h);
    hudTimer -= dt;
    if (hudTimer <= 0) {
        hudTimer = 0.05f;
        for (int p = 0; p < game.localPlayers(); p++) js_hud(p, game.hudJson(p).c_str());
    }
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int bsr_init() {
    EmscriptenWebGLContextAttributes a;
    emscripten_webgl_init_context_attributes(&a);
    a.majorVersion = 2;
    a.antialias = true;
    a.alpha = false;
    a.preserveDrawingBuffer = true;  // lets the page grab screenshots
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &a);
    if (ctx <= 0) return 0;
    emscripten_webgl_make_context_current(ctx);
    emscripten_webgl_enable_extension(ctx, "EXT_texture_filter_anisotropic");
    game.init();
    game.cb.audio = js_audio;
    game.cb.event = js_event;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, onKey);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, onKey);
    lastT = emscripten_get_now() / 1000.0;
    emscripten_set_main_loop(frame, 0, false);
    return 1;
}

// skins: comma-separated texture names, assigned to racers in order.
// roles: see Game::start ("" = one local player plus `opponents` AI cars).
// Returns the number of cars on the grid, 0 on failure.
EMSCRIPTEN_KEEPALIVE int bsr_start(const char* track, const char* carClass, const char* skins, int opponents, int laps,
                                   const char* topSpeedKmh, const char* weather, const char* roles) {
    std::vector<std::string> list;
    std::string s = skins;
    for (size_t p = 0; p <= s.size();) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        if (e > p) list.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    memset(keys, 0, sizeof keys);
    memset(steerSmooth, 0, sizeof steerSmooth);
    lastT = emscripten_get_now() / 1000.0;
    if (!game.start(track, carClass, list, opponents, laps, (float)atof(topSpeedKmh), weather, roles)) return 0;
    return (int)game.racers().size();
}

EMSCRIPTEN_KEEPALIVE void bsr_touch(float steer, float throttle, float brake) {
    touchSteer = steer;
    touchThrottle = throttle;
    touchBrake = brake;
}
EMSCRIPTEN_KEEPALIVE void bsr_set_paused(int p) { game.paused = p != 0; }
EMSCRIPTEN_KEEPALIVE void bsr_camera(int player) { game.cycleCamera(player); }
EMSCRIPTEN_KEEPALIVE void bsr_reset_car(int player) { game.resetPlayer(player); }
EMSCRIPTEN_KEEPALIVE void bsr_autopilot(int on) { game.autopilot = on != 0; }
EMSCRIPTEN_KEEPALIVE void bsr_stop() { game.stop(); }
EMSCRIPTEN_KEEPALIVE const char* bsr_results() {
    static std::string r;
    r = game.resultsJson();
    return r.c_str();
}

// ---- online
EMSCRIPTEN_KEEPALIVE void bsr_set_online(int on) { online = on != 0; }
EMSCRIPTEN_KEEPALIVE void bsr_set_waiting(int w) { game.waiting = w != 0; }
EMSCRIPTEN_KEEPALIVE int bsr_snapshot_floats() { return kSnapFloats; }
// Writes racer's state into out (bsr_snapshot_floats() floats); returns 0 if that car isn't simulated here.
EMSCRIPTEN_KEEPALIVE int bsr_get_state(int racer, float* out) {
    return game.packState(racer, emscripten_get_now() / 1000.0, out) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void bsr_put_state(int racer, const float* snap) {
    game.pushSnapshot(racer, snap, emscripten_get_now() / 1000.0);
}
EMSCRIPTEN_KEEPALIVE void bsr_remove_racer(int racer) { game.removeRacer(racer); }

}  // extern "C"

int main() { return 0; }
