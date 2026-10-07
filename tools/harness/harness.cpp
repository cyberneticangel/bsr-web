// Native test harness: runs the game headless (EGL surfaceless + Mesa) with the player on autopilot,
// saves screenshots and prints race statistics.
//
// usage: harness <data dir> <track> <class> <out dir> <race seconds> [shot times...]
//        options via env: CAM=0|1|2, OPP=n, LAPS=n, TOP=kmh, SKINS=a,b,c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/game.h"

using namespace bsr;

static void writePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = h - 1; y >= 0; y--) {  // GL origin is bottom-left
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
            raw.insert(raw.end(), p, p + 3);
        }
    }
    uLongf clen = compressBound(raw.size());
    std::vector<uint8_t> comp(clen);
    compress2(comp.data(), &clen, raw.data(), raw.size(), 6);
    comp.resize(clen);
    FILE* f = fopen(path.c_str(), "wb");
    auto be32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; fwrite(b, 1, 4, f); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        be32((uint32_t)d.size());
        fwrite(type, 1, 4, f);
        if (!d.empty()) fwrite(d.data(), 1, d.size(), f);
        uLong crc = crc32(0, (const Bytef*)type, 4);
        if (!d.empty()) crc = crc32(crc, d.data(), (uInt)d.size());
        be32((uint32_t)crc);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", comp);
    chunk("IEND", {});
    fclose(f);
}

static bool initEGL() {
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, nullptr, nullptr)) { fprintf(stderr, "eglInitialize failed\n"); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n);
    EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return false; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "eglMakeCurrent failed\n"); return false; }
    fprintf(stderr, "GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

static int gEvents = 0;
static void onEvent(const char* e) {
    gEvents++;
    if (strcmp(e, "beep") != 0) fprintf(stderr, "  event: %s\n", e);
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: harness <data dir> <track> <class> <out dir> <seconds> [shot times...]\n");
        return 2;
    }
    gDataRoot = std::string(argv[1]) + "/";
    std::string track = argv[2], cls = argv[3], out = argv[4];
    float seconds = (float)atof(argv[5]);
    std::vector<float> shots;
    for (int i = 6; i < argc; i++) shots.push_back((float)atof(argv[i]));
    if (!initEGL()) return 1;

    const int W = 960, H = 540;
    GLuint fbo, col, dep;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &col);
    glBindRenderbuffer(GL_RENDERBUFFER, col);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, col);
    glGenRenderbuffers(1, &dep);
    glBindRenderbuffer(GL_RENDERBUFFER, dep);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep);

    Game game;
    game.init();
    game.cb.event = onEvent;
    game.autopilot = getenv("MANUAL") == nullptr;
    game.debugLines = getenv("DEBUG") != nullptr;
    if (getenv("CAM")) game.setCamera(atoi(getenv("CAM")));
    std::vector<std::string> skins;
    std::string sk = getenv("SKINS") ? getenv("SKINS") : "";
    for (size_t p = 0; p < sk.size();) {
        size_t e = sk.find(',', p);
        if (e == std::string::npos) e = sk.size();
        skins.push_back(sk.substr(p, e - p));
        p = e + 1;
    }
    int opp = getenv("OPP") ? atoi(getenv("OPP")) : 5;
    int laps = getenv("LAPS") ? atoi(getenv("LAPS")) : 3;
    float top = getenv("TOP") ? (float)atof(getenv("TOP")) : 58;
    std::string wx = getenv("WEATHER") ? getenv("WEATHER") : "sunny_bluesky_summer";
    if (!game.start(track, cls, skins, opp, laps, top, wx)) { fprintf(stderr, "start failed\n"); return 1; }

    const float dt = 1.0f / 60.0f;
    size_t nextShot = 0;
    float maxSpeed = 0;
    int shotIdx = 0;
    Input in;
    if (getenv("MANUAL")) in.throttle = 1;
    for (float t = 0; t < seconds; t += dt) {
        game.update(dt, in);
        maxSpeed = std::fmax(maxSpeed, std::fabs(game.racers()[0].car.speed()));
        if (getenv("TRACE") && fmodf(t, 0.5f) < dt) {
            int ti = getenv("TRACE_CAR") ? atoi(getenv("TRACE_CAR")) : 0;
            const Car& c = game.racers()[ti].car;
            fprintf(stderr, "  [car%d pos=(%.1f,%.1f) thr=%.2f brk=%.2f steer=%.2f pathIdx=%d]", ti, c.pos.x, c.pos.y,
                    c.throttle, c.brake, c.steer, game.racers()[ti].pathIdx);
            int contacts = 0;
            for (auto& w : c.wheels) contacts += w.contact;
            fprintf(stderr, "  t=%.1f v=%.1fkm/h z=%.3f up=%.2f contacts=%d comp=%.3f,%.3f,%.3f,%.3f\n", t, c.speed() * 3.6f, c.pos.z,
                    c.R.col(2).z, contacts, c.wheels[0].comp, c.wheels[1].comp, c.wheels[2].comp, c.wheels[3].comp);
        }
        if (nextShot < shots.size() && t >= shots[nextShot]) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            game.render(W, H);
            std::vector<uint8_t> px((size_t)W * H * 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            char name[64];
            snprintf(name, sizeof name, "/%s_%02d.png", track.c_str(), shotIdx++);
            writePNG(out + name, W, H, px);
            fprintf(stderr, "  shot %s at %.1fs  hud=%s\n", name + 1, t, game.hudJson().c_str());
            nextShot++;
        }
        bool allDone = true;
        for (auto& r : game.racers()) allDone &= r.finished;
        if (allDone) break;
    }
    printf("track=%s class=%s t=%.1fs maxSpeed=%.1f km/h events=%d\n", track.c_str(), cls.c_str(), game.raceTime(),
           maxSpeed * 3.6f, gEvents);
    for (size_t i = 0; i < game.racers().size(); i++) {
        const Racer& r = game.racers()[i];
        printf("  car%zu place=%d lap=%d nextCp=%d finished=%d time=%.2f best=%.2f pos=(%.1f,%.1f,%.2f) up=%.2f\n", i,
               r.place, r.lap, r.nextCp, r.finished, r.finishTime, r.bestLap, r.car.pos.x, r.car.pos.y, r.car.pos.z,
               r.car.R.col(2).z);
    }
    return 0;
}
