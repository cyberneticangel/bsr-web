// Big Scale Racing — browser entry point (Emscripten): input, canvas, JS bridge.
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"

using namespace bsr;

EM_JS(void, js_hud, (const char* json), { if (Module.onHud) Module.onHud(UTF8ToString(json)); });
EM_JS(void, js_audio, (float rpm, float throttle, float skid, float impact, float speed),
      { if (Module.onAudio) Module.onAudio(rpm, throttle, skid, impact, speed); });
EM_JS(void, js_event, (const char* name), { if (Module.onEvent) Module.onEvent(UTF8ToString(name)); });

namespace {

Game game;
double lastT = 0;
float hudTimer = 0, steerSmooth = 0;
float touchSteer = 0, touchThrottle = 0, touchBrake = 0;

enum Key { kUp, kDown, kLeft, kRight, kW, kS, kA, kD, kSpace, kC, kR, kP, kF2, kCount };
bool keys[kCount] = {};

int keyIndex(const char* code) {
    static const char* names[kCount] = {"ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "KeyW", "KeyS", "KeyA",
                                        "KeyD", "Space", "KeyC", "KeyR", "KeyP", "F2"};
    for (int i = 0; i < kCount; i++)
        if (!strcmp(code, names[i])) return i;
    return -1;
}

EM_BOOL onKey(int type, const EmscriptenKeyboardEvent* e, void*) {
    int k = keyIndex(e->code);
    if (k < 0) return EM_FALSE;
    bool down = type == EMSCRIPTEN_EVENT_KEYDOWN;
    if (down && !e->repeat) {
        if (k == kC) game.cycleCamera();
        if (k == kF2) game.debugLines = !game.debugLines;
        if (k == kR) game.resetPlayer();
        if (k == kP && (game.state() == Game::Racing || game.state() == Game::Countdown)) {
            game.paused = !game.paused;
            js_event(game.paused ? "paused" : "resumed");
        }
    }
    keys[k] = down;
    return k <= kSpace ? EM_TRUE : EM_FALSE;  // swallow driving keys so the page doesn't scroll
}

Input readInput(float dt) {
    Input in;
    float thr = (keys[kUp] || keys[kW]) ? 1.0f : 0.0f;
    float brk = (keys[kDown] || keys[kS]) ? 1.0f : 0.0f;
    float st = ((keys[kRight] || keys[kD]) ? 1.0f : 0.0f) - ((keys[kLeft] || keys[kA]) ? 1.0f : 0.0f);
    bool analog = false;
    in.handbrake = keys[kSpace];
    // Sampling must come first: get_num_gamepads reads the sampled state.
    int np = emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS ? emscripten_get_num_gamepads() : 0;
    if (np > 0) {
        for (int i = 0; i < np; i++) {
            EmscriptenGamepadEvent gp;
            if (emscripten_get_gamepad_status(i, &gp) != EMSCRIPTEN_RESULT_SUCCESS || !gp.connected) continue;
            if (gp.numAxes > 0 && std::fabs(gp.axis[0]) > 0.15f) { st = (float)gp.axis[0]; analog = true; }
            if (gp.numButtons > 7) {
                thr = std::fmax(thr, (float)gp.analogButton[7]);
                brk = std::fmax(brk, (float)gp.analogButton[6]);
            }
            if (gp.numButtons > 0 && gp.digitalButton[0]) thr = 1;
            if (gp.numButtons > 2 && gp.digitalButton[2]) brk = 1;
            if (gp.numButtons > 1 && gp.digitalButton[1]) in.handbrake = true;
        }
    }
    if (std::fabs(touchSteer) > 0.02f) { st = touchSteer; analog = true; }
    thr = std::fmax(thr, touchThrottle);
    brk = std::fmax(brk, touchBrake);
    if (analog) {
        steerSmooth = st;
    } else {  // keyboard: ramp steering like an RC transmitter wheel
        float rate = (st == 0 || st * steerSmooth < 0) ? 6.0f : 3.5f;
        steerSmooth += clampf(st - steerSmooth, -rate * dt, rate * dt);
    }
    in.steer = steerSmooth;
    in.throttle = thr;
    in.brake = brk;
    return in;
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
    game.update(dt, readInput(dt));
    game.render(w, h);
    hudTimer -= dt;
    if (hudTimer <= 0) {
        hudTimer = 0.05f;
        js_hud(game.hudJson().c_str());
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

// skins: comma-separated texture names; the first is the player's.
EMSCRIPTEN_KEEPALIVE int bsr_start(const char* track, const char* carClass, const char* skins, int opponents, int laps,
                                   const char* topSpeedKmh, const char* weather) {
    std::vector<std::string> list;
    std::string s = skins;
    for (size_t p = 0; p <= s.size();) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        if (e > p) list.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    memset(keys, 0, sizeof keys);
    steerSmooth = 0;
    lastT = emscripten_get_now() / 1000.0;
    return game.start(track, carClass, list, opponents, laps, (float)atof(topSpeedKmh), weather) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void bsr_touch(float steer, float throttle, float brake) {
    touchSteer = steer;
    touchThrottle = throttle;
    touchBrake = brake;
}
EMSCRIPTEN_KEEPALIVE void bsr_set_paused(int p) { game.paused = p != 0; }
EMSCRIPTEN_KEEPALIVE void bsr_camera() { game.cycleCamera(); }
EMSCRIPTEN_KEEPALIVE void bsr_reset_car() { game.resetPlayer(); }
EMSCRIPTEN_KEEPALIVE void bsr_autopilot(int on) { game.autopilot = on != 0; }
EMSCRIPTEN_KEEPALIVE void bsr_stop() { game.stop(); }
EMSCRIPTEN_KEEPALIVE const char* bsr_results() {
    static std::string r;
    r = game.resultsJson();
    return r.c_str();
}

}  // extern "C"

int main() { return 0; }
