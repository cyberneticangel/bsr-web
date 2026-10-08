#include "game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace bsr {

static std::string skinPrefix(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && isdigit((unsigned char)s[e - 1])) e--;
    return s.substr(0, e);
}

bool Game::init() { return gfx_.init(); }

void Game::buildCarModel(CarModel& cm, const FSO& fso, const std::map<std::string, std::string>& ov) {
    gfx_.buildModel(fso, "", cm.model, ov);
    static const char* wn[4] = {"m_whl_lf", "m_whl_rf", "m_whl_lr", "m_whl_rr"};
    for (auto& p : cm.model.parts) {
        std::string n = lower(p.name);
        bool isWheel = false;
        for (int i = 0; i < 4; i++)
            if (n == wn[i]) { cm.wheel[i] = &p; cm.wheelCentre[i] = (p.bmin + p.bmax) * 0.5f; isWheel = true; }
        if (isWheel || n.find("coll_sphere") != std::string::npos) continue;
        cm.parts.push_back(&p);
    }
}

bool Game::start(const std::string& track, const std::string& carClass, const std::vector<std::string>& skinsIn,
                 int opponents, int laps, float topSpeedKmh, const std::string& weather, const std::string& rolesIn) {
    stop();
    std::string t = lower(track);
    if (t != trackName_ || lower(weather) != weatherName_) {
        Weather wx;
        std::map<std::string, std::string> swaps;
        if (!loadWeather(weather, wx)) loadWeather("sunny_bluesky_summer", wx);
        for (auto& [from, to] : wx.replace) swaps[from] = to;
        weatherName_ = lower(weather);
        gfx_.release(track_);
        trackFso_ = FSO();
        meta_ = TrackMeta();
        trackName_.clear();
        if (!loadFSO("tracks/" + t + "_high.fso", trackFso_)) { fprintf(stderr, "track fso failed\n"); return false; }
        if (!loadFST("tracks/" + t + "_high.fst", fst_)) { fprintf(stderr, "track fst failed\n"); return false; }
        if (!meta_.load(t)) { fprintf(stderr, "track meta failed\n"); return false; }
        gfx_.buildModel(trackFso_, t, track_, swaps);
        world_.build(fst_);
        trackName_ = t;
        if (trackFso_.sun.valid) {
            const Vec3& d = trackFso_.sun.dir;
            gfx_.setSun(V3(-d.x, -d.y, -d.z), 0.62f, 0.62f);
        }
        if (wx.fog && wx.fogEnd > wx.fogStart)
            gfx_.setFog(wx.fogColor[0], wx.fogColor[1], wx.fogColor[2], wx.fogStart, wx.fogEnd);
        else
            gfx_.setFog(0.72f, 0.78f, 0.86f, 1e5f, 2e5f);
        gfx_.setTint(wx.light[0], wx.light[1], wx.light[2]);
    }
    FSO carFso;
    std::string cls = lower(carClass);
    if (!loadFSO("cars/car_" + cls + ".fso", carFso)) { fprintf(stderr, "car fso failed\n"); return false; }
    std::vector<std::string> skins;
    for (auto& s : skinsIn) skins.push_back(lower(s));
    std::string baseSkin;
    if (!skins.empty()) {
        std::string pre = skinPrefix(skins[0]);
        for (auto& m : carFso.materials)
            for (auto& tx : m.textures)
                if (skinPrefix(tx) == pre) baseSkin = tx;
    }
    CarSpec spec;
    spec.topSpeed = topSpeedKmh / 3.6f;
    if (spec.topSpeed < 5) spec.topSpeed = 14;
    spec.accel = 5.0f + spec.topSpeed * 0.35f;
    spec.grip = cls.find("hop") != std::string::npos ? 1.35f : 1.2f;
    if (cls == "monster") { spec.grip = 1.1f; spec.mass = 12; }
    spec.steerMax = 0.5f;

    std::string roles = rolesIn.empty() ? "0" + std::string(std::max(0, opponents), 'a') : rolesIn;
    laps_ = std::max(1, laps);
    int n = std::min((int)roles.size(), (int)meta_.grid.size());
    for (int i = 0; i < n; i++) {
        auto cm = std::make_unique<CarModel>();
        std::map<std::string, std::string> ov;
        if (!baseSkin.empty()) ov[baseSkin] = skins[i % skins.size()];
        buildCarModel(*cm, carFso, ov);
        Racer r;
        int slot = n - 1 - i;  // player starts at the back of the grid
        std::vector<V3> wc;
        float wr = 0.06f;
        for (int k = 0; k < 4; k++)
            if (cm->wheel[k]) { wc.push_back(cm->wheelCentre[k]); wr = (cm->wheel[k]->bmax.z - cm->wheel[k]->bmin.z) * 0.5f; }
        if (wc.size() < 4) wc = {V3(-0.16f, 0.5f, 0.06f), V3(0.16f, 0.5f, 0.06f), V3(-0.16f, 0.05f, 0.06f), V3(0.16f, 0.05f, 0.06f)};
        std::vector<CollSphere> sph;
        for (auto& p : cm->model.parts)
            if (lower(p.name).find("coll_sphere") != std::string::npos)
                sph.push_back({(p.bmin + p.bmax) * 0.5f, (p.bmax.x - p.bmin.x) * 0.5f});
        char role = roles[i];
        r.player = role >= '0' && role < '0' + kMaxLocal ? role - '0' : -1;
        r.remote = role == 'r';
        r.ai = r.player < 0 && !r.remote;
        r.aiLine = i % std::min<size_t>(5, meta_.aiLines.size());  // aii06 is an alternate (pit) line
        r.aiSkill = !r.ai ? 1.0f : 0.86f + 0.1f * (float)((i * 7) % 5) / 4.0f;
        r.car.setup(wc, wr, sph, spec);
        const GridSlot& gs = meta_.grid[slot];
        r.car.place(gs.pos, gs.heading, world_);
        r.prevPos = r.car.pos;
        r.skin = i;
        r.place = slot + 1;
        racers_.push_back(r);
        carModels_.push_back(std::move(cm));
    }
    snaps_.assign(racers_.size(), SnapshotBuffer());
    for (int p = 0; p < kMaxLocal; p++)
        for (int i = 0; i < n; i++)
            if (racers_[i].player == p) {
                View v;
                v.racer = i;
                v.camMode = defaultCam_;
                players_.push_back(v);
            }
    if (players_.empty() && n > 0) {  // nobody local on the grid: spectate the first car
        View v;
        v.camMode = defaultCam_;
        players_.push_back(v);
    }
    state_ = Countdown;
    countdown_ = 3.999f;
    lastCountdownSec_ = -1;
    raceTime_ = 0;
    paused = false;
    waiting = false;
    accum_ = 0;
    return true;
}

void Game::stop() {
    racers_.clear();
    for (auto& cm : carModels_) gfx_.release(cm->model);
    carModels_.clear();
    snaps_.clear();
    players_.clear();
    waiting = false;
    state_ = Idle;
}

void Game::cycleCamera(int p) {
    if (p < 0 || p >= (int)players_.size()) return;
    players_[p].camMode = (players_[p].camMode + 1) % 3;
    players_[p].camInit = false;
}

void Game::setCamera(int m) {
    defaultCam_ = m % 3;
    for (auto& v : players_) { v.camMode = defaultCam_; v.camInit = false; }
}

void Game::placeOnLine(Racer& r) {
    const Path& line = meta_.aiLines[r.aiLine % meta_.aiLines.size()];
    int i = line.nearest(r.car.pos, 0, 0);
    V3 d = line.dirAt(line.dist[i]);
    r.car.place(line.pts[i], std::atan2(d.y, d.x), world_);
    r.flipTime = 0;
    r.pathIdx = i;
}

void Game::resetPlayer(int p) {
    if (p < 0 || p >= (int)players_.size() || state_ == Countdown) return;
    Racer& r = racers_[players_[p].racer];
    if (r.player == p) placeOnLine(r);
}

void Game::stepPhysics(float dt) {
    bool frozen = state_ == Countdown;
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& r = racers_[i];
        if (r.remote || r.gone) continue;  // remote cars are placed from snapshots
        r.car.hold = frozen;
        r.car.step(dt, world_);
    }
    // Each machine only pushes the cars it simulates; the owner of the other car resolves its half.
    for (size_t i = 0; i < racers_.size(); i++)
        for (size_t j = i + 1; j < racers_.size(); j++) {
            Racer& a = racers_[i];
            Racer& b = racers_[j];
            if (a.gone || b.gone || (a.remote && b.remote)) continue;
            collideCars(a.car, b.car, !a.remote, !b.remote);
        }
}

void Game::updateRace(float) {
    const int ncp = (int)meta_.checkpoints.size();
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& r = racers_[i];
        if (!r.remote && crossed(r.prevPos, r.car.pos, meta_.checkpoints[r.nextCp])) {
            if (r.nextCp == 0) {
                if (r.lap > 0) {
                    r.lastLap = raceTime_ - r.lapStart;
                    if (r.bestLap == 0 || r.lastLap < r.bestLap) r.bestLap = r.lastLap;
                }
                r.lapStart = raceTime_;
                r.lap++;
                char ev[32];
                if (r.lap > laps_ && !r.finished) {
                    r.finished = true;
                    r.finishTime = raceTime_;
                    snprintf(ev, sizeof ev, "finish:%d", r.player);
                    if (r.player >= 0) emit(ev);
                } else if (r.player >= 0 && r.lap > 1 && !r.finished) {
                    snprintf(ev, sizeof ev, "%s:%d", r.lap == laps_ ? "lastlap" : "lap", r.player);
                    emit(ev);
                }
            }
            r.nextCp = (r.nextCp + 1) % ncp;
        }
        r.prevPos = r.car.pos;
    }
    // Standings: finishers by time, then laps, checkpoints, distance to the next checkpoint.
    auto score = [&](const Racer& r) {
        const Gate& g = meta_.checkpoints[r.nextCp];
        V3 mid = (g.a + g.b) * 0.5f;
        int cps = r.nextCp == 0 ? ncp : r.nextCp;
        return r.lap * 1000.0 + cps * 10.0 - std::min(9.9f, len(mid - r.car.pos) * 0.05f);
    };
    std::vector<int> order(racers_.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const Racer& A = racers_[a];
        const Racer& B = racers_[b];
        if (A.finished != B.finished) return A.finished;
        if (A.finished) return A.finishTime < B.finishTime;
        if (A.gone != B.gone) return B.gone;  // left without finishing: last
        return score(A) > score(B);
    });
    for (size_t i = 0; i < order.size(); i++) racers_[order[i]].place = (int)i + 1;
}

void Game::update(float dt, const Input* in, int numInputs) {
    camDt_ += dt;
    if (racers_.empty() || paused) {
        if (cb.audio)
            for (int p = 0; p < (int)players_.size(); p++) cb.audio(p, 0, 0, 0, 0, 0);
        return;
    }
    if (state_ == Countdown && !waiting) {
        countdown_ -= dt;
        int sec = (int)std::ceil(countdown_);
        if (sec != lastCountdownSec_ && sec >= 0) {
            lastCountdownSec_ = sec;
            emit(sec == 0 ? "go" : "beep");
        }
        if (countdown_ <= 0) state_ = Racing;
    } else if (state_ != Countdown) {
        raceTime_ += dt;
    }
    const int ncp = (int)meta_.checkpoints.size();
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& r = racers_[i];
        if (r.gone) continue;
        if (r.remote) {
            if (snaps_[i].apply(r, now_)) r.nextCp = std::min(std::max(r.nextCp, 0), std::max(ncp - 1, 0));
            continue;
        }
        if (r.player < 0 || r.finished || autopilot) {
            driveAI(r, meta_, dt);
        } else {
            Input none;
            const Input& pi = r.player < numInputs ? in[r.player] : none;
            r.car.throttle = pi.throttle;
            r.car.brake = pi.brake;
            r.car.steer = pi.steer;
            r.car.handbrake = pi.handbrake;
        }
    }
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& r = racers_[i];
        if (r.remote || r.gone) continue;
        if (r.car.R.col(2).z < 0.2f && len(r.car.vel) < 1.0f) r.flipTime += dt;
        else r.flipTime = 0;
        if (r.flipTime > (r.player >= 0 ? 1.5f : 1.0f) || r.car.pos.z < world_.bmin.z - 5 || r.needsReset) {
            r.needsReset = false;
            placeOnLine(r);
        }
    }
    accum_ += dt;
    const float step = 1.0f / 240.0f;
    float maxImpact[kMaxLocal] = {};
    while (accum_ >= step) {
        stepPhysics(step);
        for (int p = 0; p < (int)players_.size(); p++)
            maxImpact[p] = std::max(maxImpact[p], racers_[players_[p].racer].car.impact);
        accum_ -= step;
    }
    if (state_ != Countdown) updateRace(dt);
    bool allDone = true, anyLocal = false;
    for (auto& v : players_) {
        const Racer& r = racers_[v.racer];
        if (r.player < 0) continue;
        anyLocal = true;
        allDone &= r.finished;
    }
    if (anyLocal && allDone && state_ == Racing) state_ = Finished;
    if (cb.audio)
        for (int p = 0; p < (int)players_.size(); p++) {
            const Car& c = racers_[players_[p].racer].car;
            cb.audio(p, c.rpm, c.throttle, c.skid, maxImpact[p], std::fabs(c.speed()));
        }
}

void Game::updateCamera(View& v, float dt, int x, int y, int w, int h) {
    const Car& c = racers_[v.racer].car;
    V3 fwd = c.R.col(1);
    V3 flatF = norm(V3(fwd.x, fwd.y, 0));
    V3 eye, at;
    if (v.camMode == 2) {  // bumper
        eye = c.pos + fwd * (0.25f * std::max(1.0f, c.boundRadius / 0.42f)) + c.R.col(2) * 0.2f;
        at = eye + fwd * 2.0f;
    } else {
        float scale = std::max(1.0f, c.boundRadius / 0.42f);  // bigger cars (monster trucks) need a wider view
        float dist = (v.camMode == 0 ? 1.25f : 2.3f) * scale, hgt = (v.camMode == 0 ? 0.45f : 0.9f) * scale;
        V3 vflat(c.vel.x, c.vel.y, 0);
        V3 dir = len(vflat) > 2.0f && dot(vflat, flatF) > 0 ? norm(vflat * 0.5f + flatF) : flatF;
        V3 want = c.pos - dir * dist + V3(0, 0, hgt);
        if (!v.camInit) { v.camPos = want; v.camInit = true; }
        v.camPos += (want - v.camPos) * (1.0f - std::exp(-dt * 7.0f));
        float gz = world_.groundHeight(v.camPos.x, v.camPos.y, v.camPos.z + 1.0f);
        if (v.camPos.z < gz + 0.12f) v.camPos.z = gz + 0.12f;
        eye = v.camPos;
        at = c.pos + V3(0, 0, 0.12f) + flatF * 0.3f;
    }
    M4 view = M4::lookAt(eye, at, V3(0, 0, 1));
    float aspect = (float)w / std::max(h, 1);
    float fovy = v.camMode == 2 ? 1.2f : 1.05f;
    // Split-screen views are very wide: cap the horizontal field of view instead of stretching it.
    const float kMaxAspect = 2.2f;
    if (aspect > kMaxAspect) fovy = 2 * std::atan(std::tan(fovy * 0.5f) * kMaxAspect / aspect);
    M4 proj = M4::perspective(fovy, aspect, 0.05f, 900.0f);
    gfx_.beginFrame(x, y, w, h, view, proj, eye);
}

void Game::draw() {
    for (auto& p : track_.parts) gfx_.drawPart(p, M4(), Pass::Opaque);
    for (size_t i = 0; i < racers_.size(); i++) {
        const Racer& r = racers_[i];
        if (r.gone) continue;
        CarModel& cm = *carModels_[i];
        M4 body = r.car.bodyMatrix();
        for (Part* p : cm.parts) gfx_.drawPart(*p, body, Pass::Opaque);
        for (int w = 0; w < 4 && w < (int)r.car.wheels.size(); w++)
            if (cm.wheel[w]) gfx_.drawPart(*cm.wheel[w], r.car.wheelMatrix(w, cm.wheelCentre[w]), Pass::Opaque);
    }
    for (auto& p : track_.parts) gfx_.drawPart(p, M4(), Pass::Blend);
    for (size_t i = 0; i < racers_.size(); i++) {
        if (racers_[i].gone) continue;
        M4 body = racers_[i].car.bodyMatrix();
        for (Part* p : carModels_[i]->parts) gfx_.drawPart(*p, body, Pass::Blend);
    }
    if (debugLines) {
        std::vector<float> l;
        auto add = [&](const V3& a, const V3& b) { l.insert(l.end(), {a.x, a.y, a.z + 0.05f, b.x, b.y, b.z + 0.05f}); };
        for (auto& g : meta_.checkpoints) add(g.a, g.b);
        gfx_.drawLines(l, 1, 1, 0);
        l.clear();
        const Path& p = meta_.aiLines[0];
        for (size_t i = 0; i < p.pts.size(); i++) add(p.pts[i], p.pts[(i + 1) % p.pts.size()]);
        gfx_.drawLines(l, 0, 1, 0);
    }
}

void Game::render(int w, int h) {
    if (racers_.empty() || players_.empty()) return;
    int nv = (int)players_.size();
    int vh = h / nv;
    for (int v = 0; v < nv; v++) {
        updateCamera(players_[v], camDt_, 0, h - (v + 1) * vh, w, vh);  // GL origin is bottom-left; player 0 on top
        draw();
    }
    camDt_ = 0;
}

std::string Game::hudJson(int pl) const {
    if (pl < 0 || pl >= (int)players_.size()) return "{}";
    const Racer& p = racers_[players_[pl].racer];
    char buf[512];
    int cd = state_ == Countdown ? (int)std::ceil(countdown_) : 0;
    snprintf(buf, sizeof buf,
             "{\"speed\":%.1f,\"lap\":%d,\"laps\":%d,\"place\":%d,\"racers\":%d,\"time\":%.2f,\"lapTime\":%.2f,"
             "\"best\":%.2f,\"last\":%.2f,\"countdown\":%d,\"state\":%d,\"finished\":%s,\"cam\":%d,\"waiting\":%s}",
             std::fabs(p.car.speed()) * 3.6f, std::max(1, std::min(p.lap, laps_)), laps_, p.place, (int)racers_.size(),
             raceTime_, p.lap > 0 ? raceTime_ - p.lapStart : 0.0f, p.bestLap, p.lastLap, cd, (int)state_,
             p.finished ? "true" : "false", players_[pl].camMode, waiting ? "true" : "false");
    return buf;
}

std::string Game::resultsJson() const {
    std::string out = "[";
    std::vector<const Racer*> rs;
    for (auto& r : racers_) rs.push_back(&r);
    std::sort(rs.begin(), rs.end(), [](const Racer* a, const Racer* b) { return a->place < b->place; });
    for (size_t i = 0; i < rs.size(); i++) {
        const Racer& r = *rs[i];
        char b[256];
        snprintf(b, sizeof b,
                 "%s{\"i\":%d,\"player\":%s,\"human\":%d,\"remote\":%s,\"gone\":%s,\"place\":%d,\"finished\":%s,"
                 "\"time\":%.2f,\"best\":%.2f,\"lap\":%d}",
                 i ? "," : "", (int)(rs[i] - racers_.data()), r.player >= 0 ? "true" : "false", r.player,
                 r.remote ? "true" : "false", r.gone ? "true" : "false", r.place, r.finished ? "true" : "false",
                 r.finishTime, r.bestLap, r.lap);
        out += b;
    }
    return out + "]";
}

bool Game::packState(int racer, double now, float* out) const {
    if (racer < 0 || racer >= (int)racers_.size()) return false;
    const Racer& r = racers_[racer];
    if (r.remote || r.gone) return false;
    packSnapshot(r, (float)now, out);
    return true;
}

void Game::pushSnapshot(int racer, const float* snap, double now) {
    if (racer < 0 || racer >= (int)racers_.size() || !racers_[racer].remote || racers_[racer].gone) return;
    snaps_[racer].push(snap, now);
}

void Game::removeRacer(int racer) {
    if (racer >= 0 && racer < (int)racers_.size()) racers_[racer].gone = true;
}

}  // namespace bsr
