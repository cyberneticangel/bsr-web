#include "game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
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
        if (n.find("shadow") != std::string::npos) { cm.shadow.push_back(&p); continue; }
        cm.parts.push_back(&p);
    }
    cm.bodyLo = V3(1e9f, 1e9f, 1e9f);
    cm.bodyHi = V3(-1e9f, -1e9f, -1e9f);
    for (const Part* p : cm.parts) {
        bool solid = false;
        for (const Batch& b : p->batches) solid |= !b.blend;
        if (!solid) continue;  // the shadow quad lies on the ground on purpose
        for (int k = 0; k < 8; k++) {
            V3 c = p->local.point(V3((k & 1) ? p->bmax.x : p->bmin.x, (k & 2) ? p->bmax.y : p->bmin.y, (k & 4) ? p->bmax.z : p->bmin.z));
            cm.bodyLo = V3(std::min(cm.bodyLo.x, c.x), std::min(cm.bodyLo.y, c.y), std::min(cm.bodyLo.z, c.z));
            cm.bodyHi = V3(std::max(cm.bodyHi.x, c.x), std::max(cm.bodyHi.y, c.y), std::max(cm.bodyHi.z, c.z));
        }
    }
}

bool parseRaceConfig(const std::string& s, RaceConfig& c) {
    auto split = [](const std::string& str, char sep) {
        std::vector<std::string> out;
        size_t p = 0;
        for (;;) {
            size_t e = str.find(sep, p);
            out.push_back(str.substr(p, e == std::string::npos ? std::string::npos : e - p));
            if (e == std::string::npos) return out;
            p = e + 1;
        }
    };
    for (const std::string& kv : split(s, ';')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "track") c.track = v;
        else if (k == "weather") c.weather = v;
        else if (k == "wxcat") c.weatherCat = v;
        else if (k == "laps") c.laps = atoi(v.c_str());
        else if (k == "time") c.seconds = (float)atof(v.c_str());
        else if (k == "practice") c.practice = v == "1";
        else if (k == "ghost") c.ghost = atoi(v.c_str());
        else if (k == "cars") {
            c.cars.clear();
            for (const std::string& car : split(v, '|')) {
                std::vector<std::string> f = split(car, ',');
                if (f.size() < 4 || f[0].empty() || f[3].empty()) continue;
                c.cars.push_back({f[0], f[1], (float)atof(f[2].c_str()), f[3][0], f.size() > 4 ? (float)atof(f[4].c_str()) : 0});
            }
        }
    }
    return !c.track.empty() && !c.cars.empty();
}

bool Game::start(const std::string& track, const std::string& carClass, const std::vector<std::string>& skins,
                 int opponents, int laps, float topSpeedKmh, const std::string& weather) {
    RaceConfig c;
    c.track = track;
    c.weather = weather;
    c.laps = laps;
    for (int i = 0; i <= std::max(0, opponents); i++)
        c.cars.push_back({carClass, skins.empty() ? "" : skins[i % skins.size()], topSpeedKmh, i == 0 ? '0' : 'a'});
    return start(c);
}

Game::CarModel* Game::carModel(const std::string& cls, const std::string& skinIn) {
    auto it = carFsos_.find(cls);
    if (it == carFsos_.end()) {
        FSO f;
        if (!loadFSO("cars/car_" + cls + ".fso", f)) { fprintf(stderr, "car fso failed: %s\n", cls.c_str()); return nullptr; }
        it = carFsos_.emplace(cls, std::move(f)).first;
    }
    const FSO& fso = it->second;
    // The skin replaces the class's default body texture: the one with the same name apart from the number.
    std::string skin = lower(skinIn), baseSkin;
    if (!skin.empty())
        for (auto& m : fso.materials)
            for (auto& tx : m.textures)
                if (skinPrefix(tx) == skinPrefix(skin)) baseSkin = tx;
    std::map<std::string, std::string> ov;
    if (!baseSkin.empty()) ov[baseSkin] = skin;
    auto cm = new CarModel();
    buildCarModel(*cm, fso, ov);
    return cm;
}

bool Game::start(const RaceConfig& cfgIn) {
    stop();
    if (menuEnvOn_) {  // back to the race's lighting; a new track sets its own below
        gfx_.setEnv(raceEnv_);
        menuEnvOn_ = false;
    }
    cfg_ = cfgIn;
    const std::string t = lower(cfg_.track), weather = cfg_.weather, wxKey = lower(weather + "|" + cfg_.weatherCat);
    // Presets are named <kind>_..._<W|NW>_<R|NR>_<T|NT>: wet track, rain, thunder.
    std::string wl = lower(weather), cat = lower(cfg_.weatherCat);
    rain_ = cat == "rain" || wl.find("_r_") != std::string::npos || wl.rfind("rain", 0) == 0;
    wet_ = rain_ || cat == "wet" || wl.rfind("wet", 0) == 0;
    thunder_ = rain_ && wl.size() > 2 && wl.compare(wl.size() - 2, 2, "_t") == 0;
    if (t != trackName_ || wxKey != weatherName_) {
        Weather wx;
        std::map<std::string, std::string> swaps;
        if (!loadWeather(weather, wx)) loadWeather("sunny_bluesky_summer", wx);
        for (auto& [from, to] : wx.replace) swaps[from] = to;
        TrackWeather tw;  // e.g. rain: dry lightmaps -> wet ones, and a darker sky dome
        if (!cat.empty() && loadTrackWeather(t, cat, tw))
            for (auto& [from, to] : tw.swaps) swaps[from] = to;
        weatherName_ = wxKey;
        gfx_.release(track_);
        gfx_.release(prizes_);
        trackFso_ = FSO();
        meta_ = TrackMeta();
        trackName_.clear();
        if (!loadFSO("tracks/" + t + "_high.fso", trackFso_)) { fprintf(stderr, "track fso failed\n"); return false; }
        if (!loadFST("tracks/" + t + "_high.fst", fst_)) { fprintf(stderr, "track fst failed\n"); return false; }
        if (!meta_.load(t)) { fprintf(stderr, "track meta failed\n"); return false; }
        gfx_.buildModel(trackFso_, t, track_, swaps);
        world_.build(fst_);
        FSO prizeFso;
        if (loadFSO("prizes_track.fso", prizeFso)) gfx_.buildModel(prizeFso, "", prizes_);
        if (!loadTrackCams(t, cams_)) cams_.clear();
        speakers_.clear();
        for (const Part& p : track_.parts) {
            std::string n = lower(p.name);
            if (n.find("refl") == std::string::npos &&
                (n.find("speakerrond") != std::string::npos || n.find("speakervierkant") != std::string::npos))
                speakers_.push_back(p.local.point((p.bmin + p.bmax) * 0.5f));
        }
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
    laps_ = std::max(1, cfg_.laps);
    timeUp_ = false;
    minutesLeftSaid_ = 0;
    int n = std::min((int)cfg_.cars.size(), (int)meta_.grid.size());
    for (int i = 0; i < n; i++) {
        const CarEntry& e = cfg_.cars[i];
        std::string cls = lower(e.cls);
        std::unique_ptr<CarModel> cm(carModel(cls, e.skin));
        if (!cm) return false;
        CarSpec spec;
        spec.topSpeed = e.topSpeedKmh / 3.6f;
        if (spec.topSpeed < 5) spec.topSpeed = 14;
        spec.accel = 5.0f + spec.topSpeed * 0.35f;
        spec.grip = cls.find("hop") != std::string::npos ? 1.35f : 1.2f;
        if (cls == "monster") { spec.grip = 1.1f; spec.mass = 12; }
        spec.steerMax = 0.5f;
        if (wet_) spec.grip *= rain_ ? 0.85f : 0.9f;  // wet tarmac
        Racer r;
        r.cls = cls;
        int slot = n - 1 - i;  // cars[0] starts at the back of the grid
        std::vector<V3> wc;
        float wr = 0.06f;
        for (int k = 0; k < 4; k++)
            if (cm->wheel[k]) { wc.push_back(cm->wheelCentre[k]); wr = (cm->wheel[k]->bmax.z - cm->wheel[k]->bmin.z) * 0.5f; }
        if (wc.size() < 4) wc = {V3(-0.16f, 0.5f, 0.06f), V3(0.16f, 0.5f, 0.06f), V3(-0.16f, 0.05f, 0.06f), V3(0.16f, 0.05f, 0.06f)};
        std::vector<CollSphere> sph;
        for (auto& p : cm->model.parts)
            if (lower(p.name).find("coll_sphere") != std::string::npos)
                sph.push_back({(p.bmin + p.bmax) * 0.5f, (p.bmax.x - p.bmin.x) * 0.5f});
        const int modelSpheres = (int)sph.size();
        // The model's collision spheres sit high (their bottoms ~7 cm above the drawn underside), so on crests,
        // kerbs and the foot of banks the nose and tail sank into the ground. Skid spheres flush with the
        // underside at the four corners keep the drawn body out of it.
        if (cm->bodyLo.x < cm->bodyHi.x) {
            const float sr = 0.025f;
            for (int k = 0; k < 4; k++) {
                V3 c((k & 1) ? cm->bodyHi.x - sr : cm->bodyLo.x + sr, (k & 2) ? cm->bodyHi.y - sr : cm->bodyLo.y + sr,
                     cm->bodyLo.z + sr);
                sph.push_back({c, sr});
            }
        }
        char role = e.role;
        r.player = role >= '0' && role < '0' + kMaxLocal ? role - '0' : -1;
        r.remote = role == 'r';
        r.ai = r.player < 0 && !r.remote;
        r.aiLine = i % std::min<size_t>(5, meta_.aiLines.size());  // aii06 is an alternate (pit) line
        r.aiSkill = !r.ai ? 1.0f : e.skill > 0 ? clampf(e.skill, 0.5f, 1.0f) : 0.86f + 0.1f * (float)((i * 7) % 5) / 4.0f;
        r.car.setup(wc, wr, sph, spec);
        r.car.bodySpheres = modelSpheres > 0 ? modelSpheres : -1;
        const GridSlot& gs = meta_.grid[slot];
        r.car.place(gs.pos, gs.heading, world_);
        r.prevPos = r.car.pos;
        r.gridSlot = slot;
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
    std::vector<std::string> classes;
    for (const Racer& r : racers_) classes.push_back(r.cls);
    fx_.reset(gfx_, rain_, wet_, classes);
    ghostModel_.reset();
    ghostCur_ = GhostLap();
    ghostVisible_ = false;
    if (cfg_.ghost && !players_.empty() && racers_[players_[0].racer].player == 0) {
        const Racer& p = racers_[players_[0].racer];
        std::string key = t + "|" + p.cls;
        if (key != ghostKey_) { ghostBest_ = ghostLast_ = GhostLap(); ghostKey_ = key; }  // laps on another track/car
        ghostModel_.reset(carModel(p.cls, cfg_.cars[players_[0].racer].skin));
        ghost_ = p;
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

static const float kGhostStep = 0.05f;  // seconds of lap time between recorded ghost frames

void Game::updateGhost(float) {
    ghostVisible_ = false;
    if (players_.empty() || !ghostModel_) return;
    const Racer& p = racers_[players_[0].racer];
    if (p.player != 0 || p.lap < 1) return;
    float lt = raceTime_ - p.lapStart;
    // Record: one frame per kGhostStep of lap time (a lap that ran long is dropped, it won't be a best lap).
    if (ghostCur_.frames.size() < 20 * 60 * 5 && (int)ghostCur_.frames.size() <= (int)(lt / kGhostStep)) {
        std::vector<float> f(kSnapFloats);
        packSnapshot(p, lt, f.data());
        ghostCur_.frames.push_back(std::move(f));
    }
    // Replay the chosen lap at the same lap time.
    const GhostLap& g = cfg_.ghost == 2 ? ghostLast_ : ghostBest_;
    if (g.frames.size() < 2) return;
    float x = lt / kGhostStep;
    size_t i = (size_t)x;
    if (i + 1 >= g.frames.size()) return;  // the ghost has already finished its lap
    applySnapshot(ghost_, g.frames[i].data(), g.frames[i + 1].data(), x - (float)i);
    ghostVisible_ = true;
}

void Game::stop() {
    racers_.clear();
    for (auto& cm : carModels_) gfx_.release(cm->model);
    carModels_.clear();
    podiumTime_ = 0;
    if (ghostModel_) gfx_.release(ghostModel_->model);
    ghostModel_.reset();
    ghostVisible_ = false;
    snaps_.clear();
    players_.clear();
    waiting = false;
    state_ = Idle;
}

void Game::cycleCamera(int p) {
    if (p < 0 || p >= (int)players_.size()) return;
    players_[p].camMode = (players_[p].camMode + 1) % kCamModes;
    players_[p].camInit = false;
}

void Game::setCamera(int m) {
    defaultCam_ = ((m % kCamModes) + kCamModes) % kCamModes;
    for (auto& v : players_) { v.camMode = defaultCam_; v.camInit = false; }
}

void Game::placeOnLine(Racer& r) {
    const Path& line = meta_.aiLines[r.aiLine % meta_.aiLines.size()];
    int i = line.nearest(r.car.pos, 0, 0);
    V3 d = line.dirAt(line.dist[i]);
    r.car.place(line.pts[i], std::atan2(d.y, d.x), world_);
    r.flipTime = 0;
    r.pathIdx = i;
    r.resets++;
}

void Game::placeAtCheckpoint(Racer& r) {
    const int ncp = (int)meta_.checkpoints.size();
    if (r.lap == 0 && r.nextCp == 0) {  // start line not crossed yet: back to the grid
        const GridSlot& gs = meta_.grid[r.gridSlot % meta_.grid.size()];
        r.car.place(gs.pos, gs.heading, world_);
    } else {
        // Where the racing line crosses the last checkpoint passed in order, heading along the line.
        const Gate& g = meta_.checkpoints[(r.nextCp + ncp - 1) % ncp];
        const Path& line = meta_.aiLines[0];
        const int n = (int)line.pts.size();
        V3 at = (g.a + g.b) * 0.5f;
        float d = line.dist[line.nearest(at, 0, 0)];
        for (int i = 0; i < n; i++) {
            const V3& p0 = line.pts[i];
            const V3& p1 = line.pts[(i + 1) % n];
            if (!crossed(p0, p1, g)) continue;
            V3 e = g.b - g.a, s = p1 - p0;
            float den = s.x * e.y - s.y * e.x;
            float t = std::fabs(den) > 1e-6f ? ((g.a.x - p0.x) * e.y - (g.a.y - p0.y) * e.x) / den : 0.5f;
            at = p0 + s * t;
            d = line.dist[i] + len(s) * t;
            break;
        }
        V3 dir = line.dirAt(d);
        r.car.place(at, std::atan2(dir.y, dir.x), world_);
        r.pathIdx = line.nearest(r.car.pos, 0, 0);
    }
    r.prevPos = r.car.pos;  // the jump must not count as crossing any checkpoint
    r.flipTime = r.stuckTime = r.reverseTime = 0;
    r.resets++;
}

void Game::resetPlayer(int p) {
    if (p < 0 || p >= (int)players_.size() || state_ == Countdown) return;
    Racer& r = racers_[players_[p].racer];
    if (r.player == p) placeAtCheckpoint(r);
}

std::vector<int> Game::missedCheckpoints(const Racer& r) const {
    std::vector<int> out;
    const int ncp = (int)meta_.checkpoints.size();
    if (r.cutTo < 0 || ncp == 0) return out;
    for (int g = r.nextCp; g != r.cutTo; g = (g + 1) % ncp)
        if (g >= (int)r.cutPassed.size() || !r.cutPassed[g]) out.push_back(g);
    return out;
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

void Game::emitf(const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(buf);
}

// Called when the racer crosses the start/finish line in the right order.
void Game::finishLap(Racer& r) {
    bool ghostCar = cfg_.ghost && r.player == 0;
    if (r.lap > 0) {
        r.lastLap = raceTime_ - r.lapStart;
        if (r.bestLap == 0 || r.lastLap < r.bestLap) r.bestLap = r.lastLap;
        if (r.player >= 0 && !r.finished) emitf("laptime:%d:%.3f", r.player, r.lastLap);  // not autopilot laps
        if (ghostCar && !ghostCur_.frames.empty()) {
            ghostCur_.time = r.lastLap;
            if (ghostBest_.frames.empty() || ghostCur_.time < ghostBest_.time) ghostBest_ = ghostCur_;
            ghostLast_ = ghostCur_;
        }
    }
    if (ghostCar) ghostCur_.frames.clear();
    r.lapStart = raceTime_;
    r.lap++;
    if (r.finished) return;
    bool done = cfg_.practice ? false : cfg_.seconds > 0 ? timeUp_ && r.lap > 1 : r.lap > laps_;
    if (done) {
        r.finished = true;
        r.finishTime = raceTime_;
        if (r.player >= 0) emitf("finish:%d", r.player);
    } else if (r.player >= 0 && r.lap > 1) {
        emitf("%s:%d", cfg_.seconds <= 0 && !cfg_.practice && r.lap == laps_ ? "lastlap" : "lap", r.player);
    }
}

// Local players get told when they cut past a checkpoint or cross one out of order.
void Game::checkpointWarnings(Racer& r, float dt) {
    r.cpWarnTime = std::max(0.0f, r.cpWarnTime - dt);
    const int ncp = (int)meta_.checkpoints.size();
    if (r.player < 0 || r.finished || ncp < 3) return;
    for (int g = 0; g < ncp; g++) {
        if (g == r.nextCp || g == (r.nextCp + ncp - 1) % ncp) continue;  // the expected one, or the one just passed
        if (!crossed(r.prevPos, r.car.pos, meta_.checkpoints[g])) continue;
        int ahead = (g - r.nextCp + ncp) % ncp;
        if (ahead <= ncp / 2) {
            if (r.cutTo < 0 || ahead > (r.cutTo - r.nextCp + ncp) % ncp) r.cutTo = g;
            r.cutPassed.resize(ncp);
            r.cutPassed[g] = 1;
        }
        if (r.cpWarnTime > 0) return;
        r.cpWarnTime = 2.5f;
        emitf("%s:%d", ahead <= ncp / 2 ? "missedcp" : "wrongcp", r.player);
        return;
    }
}

void Game::updateRace(float dt) {
    const int ncp = (int)meta_.checkpoints.size();
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& r = racers_[i];
        if (!r.remote) {
            if (crossed(r.prevPos, r.car.pos, meta_.checkpoints[r.nextCp])) {
                if (r.nextCp == 0) finishLap(r);
                r.nextCp = (r.nextCp + 1) % ncp;
                if (r.cutTo == r.nextCp) {  // went back through everything that was cut
                    r.cutTo = -1;
                    r.cutPassed.clear();
                }
            } else {
                checkpointWarnings(r, dt);
            }
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
        if (A.finished && A.lap != B.lap) return A.lap > B.lap;  // timed race: more laps beats an earlier finish
        if (A.finished) return A.finishTime < B.finishTime;
        if (A.gone != B.gone) return B.gone;  // left without finishing: last
        return score(A) > score(B);
    });
    for (size_t i = 0; i < order.size(); i++) racers_[order[i]].place = (int)i + 1;
}

void Game::update(float dt, const Input* in, int numInputs) {
    camDt_ += dt;
    if (state_ == Podium) {
        podiumTime_ += dt;
        fx_.update(dt, {}, world_, camEye_);  // rain keeps falling, smoke clears
        if (cb.audio)
            for (int p = 0; p < (int)players_.size(); p++) cb.audio(p, 0, 0, 0, 0, 0);
        return;
    }
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
    if (cfg_.seconds > 0 && state_ == Racing && !cfg_.practice) {
        float left = cfg_.seconds - raceTime_;
        for (int m = 3; m >= 1; m--)  // "3/2/1 minutes left", unless the race is barely longer than that
            if (left <= m * 60 && (minutesLeftSaid_ == 0 || minutesLeftSaid_ > m) && cfg_.seconds > m * 60 + 5) {
                minutesLeftSaid_ = m;
                emitf("timeleft:%d", m);
            }
        if (left <= 0 && !timeUp_) {
            timeUp_ = true;
            emit("timeup");
        }
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
        // A car stopped or on its roof mid-race: the announcer warns about it (once per incident).
        bool stalled = state_ == Racing && !r.finished && (r.flipTime > 0 || std::fabs(r.car.speed()) < 0.3f);
        r.slowTime = stalled ? r.slowTime + dt : 0;
        if (r.slowTime > 0.8f && !r.deadReported && raceTime_ > 3) {
            r.deadReported = true;
            emitf("deadcar:%d", r.player);
        }
        if (r.slowTime == 0) r.deadReported = false;
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
    if (cfg_.ghost) updateGhost(dt);
    if (!players_.empty()) fx_.update(dt, racers_, world_, players_[0].camPos);
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

const TrackCam* Game::findCam(const std::string& name) const {
    for (const TrackCam& c : cams_)
        if (lower(c.name) == name) return &c;
    return nullptr;
}

void Game::updateCamera(View& v, float dt, int x, int y, int w, int h) {
    const Car& c = racers_[v.racer].car;
    V3 fwd = c.R.col(1);
    V3 flatF = norm(V3(fwd.x, fwd.y, 0));
    float scale = std::max(1.0f, c.boundRadius / 0.42f);  // bigger cars (monster trucks) need a wider view
    V3 eye, at;
    float hfov = 1.6f;
    // The view this mode uses: a player camera from the track's camera file, or for TV the set-1 camera that
    // covers the car's stretch of the racing line.
    static const char* names[] = {"follow", "follow2", "follow3", "bumper", "tower"};
    const TrackCam* cam = v.camMode < CamTV ? findCam(names[v.camMode]) : nullptr;
    int type = v.camMode == CamBumper ? 2 : v.camMode == CamTower ? 1 : 3;
    if (v.camMode == CamTV && !meta_.aiLines.empty()) {
        const Path& line = meta_.aiLines[0];
        v.tvHint = line.nearest(c.pos, v.tvHint, v.camInit ? 60 : 0);
        float d = line.dist[v.tvHint];
        for (const TrackCam& tc : cams_)
            if (tc.use == 2 && tc.name.rfind("1_", 0) == 0 && d >= tc.range0 && d < tc.range1) cam = &tc;
        type = cam ? cam->type : 3;
    }
    if (cam && cam->fov > 0.2f) hfov = cam->fov;
    if (type == 2) {  // bumper
        eye = c.pos + fwd * (0.25f * scale) + c.R.col(2) * 0.2f;
        at = eye + fwd * 2.0f;
        if (!cam) hfov = 1.75f;
    } else if (type == 1) {  // fixed: the driver's stand (tower) or a trackside TV camera, zooming on the car
        V3 p = cam ? V3(cam->pos.x, cam->pos.y, cam->pos.z) : meta_.grid.empty() ? V3(0, 0, 5) : meta_.grid[0].pos + V3(0, 0, 4.9f);
        eye = p;
        at = c.pos + V3(0, 0, 0.1f * scale);
        float d = len(at - eye);
        // Keep the car a steady size: about 3 m (tower) or 4 m (TV) of track across the picture.
        float span = (v.camMode == CamTower ? 3.0f : 4.0f) * scale;
        hfov = clampf(2 * std::atan(span * 0.5f / std::max(d, 0.1f)), 0.08f, cam ? cam->fov : 1.2f);
        v.camInit = false;  // back to a chase: start there instead of flying over
    } else {  // chase: back, up and look-at height from the camera file (follow: 2.8 m back, 1.1 m up)
        static const float defaults[3][3] = {{2.8f, 1.1f, 0.4f}, {1.4f, 0.6f, 0.3f}, {1.0f, 0.5f, 0.2f}};
        const float* def = defaults[std::min(v.camMode, 2)];
        bool useCam = cam && cam->offset.x < -0.1f;
        float back = (useCam ? -cam->offset.x : def[0]) * scale, up = (useCam ? cam->offset.y : def[1]) * scale;
        float look = (useCam ? cam->offset.z : def[2]) * scale;
        V3 vflat(c.vel.x, c.vel.y, 0);
        V3 dir = len(vflat) > 2.0f && dot(vflat, flatF) > 0 ? norm(vflat * 0.5f + flatF) : flatF;
        V3 want = c.pos - dir * back + V3(0, 0, up);
        if (!v.camInit) { v.camPos = want; v.camInit = true; }
        v.camPos += (want - v.camPos) * (1.0f - std::exp(-dt * 7.0f));
        float gz = world_.groundHeight(v.camPos.x, v.camPos.y, v.camPos.z + 1.0f);
        if (v.camPos.z < gz + 0.12f) v.camPos.z = gz + 0.12f;
        eye = v.camPos;
        at = c.pos + V3(0, 0, look * 0.5f) + flatF * (look * 1.5f);
    }
    M4 view = M4::lookAt(eye, at, V3(0, 0, 1));
    camEye_ = eye;
    camRight_ = V3(view.m[0], view.m[4], view.m[8]);
    camUp_ = V3(view.m[1], view.m[5], view.m[9]);
    if (cb.listener && &v == &players_[0]) cb.listener(eye, norm(at - eye), V3(0, 0, 1));
    float aspect = (float)w / std::max(h, 1);
    // The original's fields of view are horizontal (at 4:3); keep them horizontal on wide screens and
    // split-screen halves, so the framing matches.
    float fovy = 2 * std::atan(std::tan(hfov * 0.5f) / std::max(aspect, 4.0f / 3.0f));
    M4 proj = M4::perspective(fovy, aspect, 0.05f, 900.0f);
    if (&v == &players_[0]) { viewProj0_ = proj * view; eye0_ = eye; }
    gfx_.beginFrame(x, y, w, h, view, proj, eye);
}

void Game::draw() {
    for (auto& p : track_.parts) gfx_.drawPart(p, M4(), Pass::Opaque);
    for (size_t i = 0; i < racers_.size(); i++) {
        const Racer& r = racers_[i];
        if (r.gone || r.hidden) continue;
        CarModel& cm = *carModels_[i];
        M4 body = r.car.bodyMatrix();
        for (Part* p : cm.parts) gfx_.drawPart(*p, body, Pass::Opaque);
        for (int w = 0; w < 4 && w < (int)r.car.wheels.size(); w++)
            if (cm.wheel[w]) gfx_.drawPart(*cm.wheel[w], r.car.wheelMatrix(w, cm.wheelCentre[w]), Pass::Opaque);
    }
    for (auto& p : track_.parts) gfx_.drawPart(p, M4(), Pass::Blend);
    if (state_ == Podium)
        for (int k = 0; k < 3 && k < (int)meta_.prizes.size(); k++) {
            const Part* cup = nullptr;
            for (const Part& p : prizes_.parts)
                if (p.name == std::to_string(k + 1)) cup = &p;
            if (!cup) continue;
            // Stand the cup's base on its prize position.
            V3 base = cup->local.point(V3((cup->bmin.x + cup->bmax.x) * 0.5f, (cup->bmin.y + cup->bmax.y) * 0.5f, cup->bmin.z));
            M4 at = M4::translate(meta_.prizes[k] - base);
            gfx_.drawPart(*cup, at, Pass::Opaque);
            gfx_.drawPart(*cup, at, Pass::Blend);
        }
    for (size_t i = 0; i < racers_.size(); i++) {
        if (racers_[i].gone || racers_[i].hidden) continue;
        M4 body = racers_[i].car.bodyMatrix();
        for (Part* p : carModels_[i]->parts) gfx_.drawPart(*p, body, Pass::Blend);
        drawCarShadow(*carModels_[i], racers_[i].car);
    }
    fx_.draw(gfx_, camEye_, camRight_, camUp_);
    if (ghostVisible_) {  // translucent, after everything else so the track shows through it
        M4 body = ghost_.car.bodyMatrix();
        for (Part* p : ghostModel_->parts) gfx_.drawPart(*p, body, Pass::Blend, 0.5f);
        drawCarShadow(*ghostModel_, ghost_.car, 0.5f);
        for (int w = 0; w < 4 && w < (int)ghost_.car.wheels.size(); w++)
            if (ghostModel_->wheel[w])
                gfx_.drawPart(*ghostModel_->wheel[w], ghost_.car.wheelMatrix(w, ghostModel_->wheelCentre[w]), Pass::Blend, 0.5f);
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

// A little clearance over the collision surface, which the drawn road can sit slightly above.
static const float kShadowLift = 0.01f;

bool Game::shadowMatrix(const Car& c, M4& out, float& alpha) const {
    Hit h;
    if (!world_.raycast(c.pos + V3(0, 0, 0.3f), V3(0, 0, -1), 2.3f, h) || h.n.z < 0.3f) return false;
    V3 ground = c.pos + V3(0, 0, 0.3f - h.t) + h.n * kShadowLift;
    float rest = c.wheels.empty() ? 0.08f : -c.wheels[0].local.z + c.wheels[0].radius;  // CoM height when parked
    alpha = clampf(1.0f - (h.t - 0.3f - rest) / 1.2f, 0.0f, 1.0f);
    if (alpha <= 0.01f) return false;
    V3 f = c.R.col(1);
    f = f - h.n * dot(f, h.n);
    if (dot(f, f) < 1e-6f) f = norm(cross(h.n, c.R.col(0)));  // nose straight up or down
    f = norm(f);
    out = M4::fromRT(M3::fromAxes(norm(cross(f, h.n)), f, h.n), ground) * M4::translate(V3(-c.comModel.x, -c.comModel.y, 0));
    return true;
}

void Game::drawCarShadow(const CarModel& cm, const Car& c, float alphaMul) {
    M4 m;
    float a = 1;
    if (cm.shadow.empty() || !shadowMatrix(c, m, a)) return;
    for (Part* p : cm.shadow) gfx_.drawPart(*p, m, Pass::Blend, a * alphaMul);
}

bool Game::startPodium() {
    if (racers_.empty() || meta_.podium.size() < 3 || state_ == Idle) return false;
    std::vector<Racer*> order;
    for (Racer& r : racers_) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) { return a->place < b->place; });
    for (size_t k = 0; k < order.size(); k++) {
        Racer& r = *order[k];
        r.hidden = k >= meta_.podium.size() || r.gone;
        if (r.hidden) continue;
        const GridSlot& g = meta_.podium[k];
        Car& c = r.car;
        V3 f(std::cos(g.heading), std::sin(g.heading), 0);
        c.R = M3::fromAxes(norm(cross(f, V3(0, 0, 1))), f, V3(0, 0, 1));
        float wheelZ = c.wheels.empty() ? 0.06f : -c.wheels[0].local.z + c.wheels[0].radius;
        c.pos = g.pos + V3(0, 0, wheelZ + 0.003f);  // exactly on the step: no physics on the podium
        c.vel = c.angVel = V3();
        for (Wheel& w : c.wheels) { w.comp = 0; w.steer = 0; }
    }
    ghostVisible_ = false;
    paused = false;
    state_ = Podium;
    podiumTime_ = 0;
    return true;
}

void Game::podiumCamera(int x, int y, int w, int h) {
    V3 c(0, 0, 0);
    for (int k = 0; k < 3; k++) c += meta_.podium[k].pos;
    c = c * (1.0f / 3);
    float hd = meta_.podium[0].heading + std::sin(podiumTime_ * 0.3f) * 0.45f;  // slow sweep in front of it
    float in = clampf(podiumTime_ / 2.5f, 0, 1);
    float ease = in * in * (3 - 2 * in);
    float dist = 4.8f - 1.2f * ease;  // a short move in, looking down onto the steps over the cups
    V3 eye = c + V3(std::cos(hd), std::sin(hd), 0) * dist + V3(0, 0, 2.1f - 0.4f * ease);
    V3 at = c + V3(0, 0, 0.12f);
    M4 view = M4::lookAt(eye, at, V3(0, 0, 1));
    camEye_ = eye;
    camRight_ = V3(view.m[0], view.m[4], view.m[8]);
    camUp_ = V3(view.m[1], view.m[5], view.m[9]);
    viewProj0_ = M4::perspective(0.75f, (float)w / std::max(h, 1), 0.05f, 900.0f) * view;
    eye0_ = eye;
    if (cb.listener) cb.listener(eye, norm(at - eye), V3(0, 0, 1));
    gfx_.beginFrame(x, y, w, h, view, M4::perspective(0.75f, (float)w / std::max(h, 1), 0.05f, 900.0f), eye);
}

void Game::render(int w, int h) {
    if (menuScene && menuLoaded_) {
        renderMenu(w, h);
        return;
    }
    if (menuEnvOn_) {
        gfx_.setEnv(raceEnv_);
        menuEnvOn_ = false;
    }
    if (racers_.empty() || players_.empty()) return;
    if (state_ == Podium) {
        podiumCamera(0, 0, w, h);
        draw();
        camDt_ = 0;
        return;
    }
    int nv = (int)players_.size();
    int vh = h / nv;
    vp0_[0] = 0;
    vp0_[1] = 0;
    vp0_[2] = 1;
    vp0_[3] = 1.0f / nv;
    for (int v = 0; v < nv; v++) {
        updateCamera(players_[v], camDt_, 0, h - (v + 1) * vh, w, vh);  // GL origin is bottom-left; player 0 on top
        draw();
    }
    camDt_ = 0;
}

bool Game::loadMenuScene() {
    if (menuLoaded_) return true;
    FSO fso;
    if (!loadFSO("menuscene.fso", fso)) return false;
    gfx_.buildModel(fso, "", menu_);
    menuController_.clear();
    menuWall_ = menuChecker_ = nullptr;
    for (Part& p : menu_.parts) {
        std::string n = lower(p.name);
        if (n == "m_background" || n == "m_foreground") {
            for (Batch& b : p.batches) {
                b.unlit = true;                     // flat, full-brightness backdrop
                b.blend = n == "m_foreground";      // the checker is translucent
            }
            (n == "m_background" ? menuWall_ : menuChecker_) = &p;
        } else if (n.rfind("m_controller", 0) == 0) {
            menuController_.push_back(&p);
            if (n == "m_controller") menuPivot_ = p.local.point((p.bmin + p.bmax) * 0.5f);
        }
    }
    menuEnv_ = gfx_.env();
    if (fso.sun.valid) {
        menuEnv_.sunDir = norm(V3(-fso.sun.dir.x, -fso.sun.dir.y, -fso.sun.dir.z));
        menuEnv_.amb = fso.sun.ambient[0];
        menuEnv_.diff = fso.sun.diffuse[0];
    }
    for (int k = 0; k < 3; k++) menuEnv_.fog[k] = menuEnv_.tint[k] = 1;  // white behind, no tint
    menuEnv_.fogStart = 1e5f;
    menuEnv_.fogEnd = 2e5f;
    menuLoaded_ = true;
    return true;
}

void Game::renderMenu(int w, int h) {
    float dt = menuNow_ < 0 ? 0.0f : (float)std::min(0.1, std::max(0.0, now_ - menuNow_));
    menuNow_ = now_;
    menuTime_ += dt;
    if (!menuEnvOn_) {
        raceEnv_ = gfx_.env();
        menuEnvOn_ = true;
    }
    gfx_.setEnv(menuEnv_);
    // The camera sits at the origin looking down +Y with a 90 degree horizontal view (at 4:3): the planes are
    // one 20-unit tile wider than that on each side, the room they slide in before wrapping.
    float aspect = (float)w / std::max(h, 1);
    float fovy = 2 * std::atan(1.0f / std::max(aspect, 4.0f / 3.0f));
    V3 eye(0, 0, 0);
    gfx_.beginFrame(0, 0, w, h, M4::lookAt(eye, V3(0, 1, 0), V3(0, 0, 1)), M4::perspective(fovy, aspect, 1.0f, 200.0f), eye);
    auto wrap = [](float v) {  // into [-10, 10): the planes repeat every 20 units
        v = std::fmod(v + 10.0f, 20.0f);
        return (v < 0 ? v + 20.0f : v) - 10.0f;
    };
    float t = menuTime_;
    M4 wall = M4::translate(V3(wrap(-1.2f * t), 0, wrap(-0.8f * t)));
    M4 checker = M4::translate(V3(wrap(1.6f * t), 0, wrap(1.2f * t)));
    M4 spin = M4::translate(menuPivot_) * M4::rotZ(0.8f * t) * M4::translate(-menuPivot_);
    gfx_.fogOff = true;
    if (menuWall_) gfx_.drawPart(*menuWall_, wall, Pass::Opaque);
    for (Part* p : menuController_) gfx_.drawPart(*p, spin, Pass::Opaque);
    for (Part* p : menuController_) gfx_.drawPart(*p, spin, Pass::Blend);
    if (menuChecker_) gfx_.drawPart(*menuChecker_, checker, Pass::Blend);
    gfx_.fogOff = false;
}

bool Game::renderPreview(const std::string& clsIn, const std::string& skin, int w, int h, std::vector<uint8_t>& rgba) {
    std::unique_ptr<CarModel> cm(carModel(lower(clsIn), skin));
    if (!cm) return false;
    V3 lo = cm->bodyLo, hi = cm->bodyHi;
    for (int k = 0; k < 4; k++)
        if (cm->wheel[k]) {
            lo = V3(std::min(lo.x, cm->wheel[k]->bmin.x), std::min(lo.y, cm->wheel[k]->bmin.y), std::min(lo.z, cm->wheel[k]->bmin.z));
            hi = V3(std::max(hi.x, cm->wheel[k]->bmax.x), std::max(hi.y, cm->wheel[k]->bmax.y), std::max(hi.z, cm->wheel[k]->bmax.z));
        }
    V3 c = (lo + hi) * 0.5f;
    float r = len(hi - lo) * 0.5f;
    // Three-quarter view from the front right, slightly above (the model faces +Y).
    V3 eye = c + norm(V3(0.95f, 1.25f, 0.6f)) * (r * 2.05f);
    float fov = 0.75f;
    gfx_.beginOffscreen(w, h);
    gfx_.beginFrame(0, 0, w, h, M4::lookAt(eye, c, V3(0, 0, 1)), M4::perspective(fov, (float)w / h, r * 0.2f, r * 10), eye);
    gfx_.clearColor(0, 0, 0, 0);
    gfx_.fogOff = true;
    for (Part* p : cm->parts) gfx_.drawPart(*p, M4(), Pass::Opaque);
    for (int k = 0; k < 4; k++)
        if (cm->wheel[k]) gfx_.drawPart(*cm->wheel[k], M4(), Pass::Opaque);
    for (Part* p : cm->parts) gfx_.drawPart(*p, M4(), Pass::Blend);
    for (Part* p : cm->shadow) gfx_.drawPart(*p, M4(), Pass::Blend);
    gfx_.fogOff = false;
    gfx_.endOffscreen(rgba);
    gfx_.release(cm->model);
    return true;
}

std::string Game::hudJson(int pl) const {
    if (pl < 0 || pl >= (int)players_.size()) return "{}";
    const Racer& p = racers_[players_[pl].racer];
    char buf[512];
    int cd = state_ == Countdown ? (int)std::ceil(countdown_) : 0;
    snprintf(buf, sizeof buf,
             "{\"speed\":%.1f,\"lap\":%d,\"laps\":%d,\"place\":%d,\"racers\":%d,\"time\":%.2f,\"lapTime\":%.2f,"
             "\"best\":%.2f,\"last\":%.2f,\"countdown\":%d,\"state\":%d,\"finished\":%s,\"cam\":%d,\"waiting\":%s,"
             "\"timeLeft\":%.2f,\"practice\":%s,\"rpm\":%.2f,\"top\":%.1f}",
             std::fabs(p.car.speed()) * 3.6f, std::max(1, cfg_.seconds > 0 || cfg_.practice ? p.lap : std::min(p.lap, laps_)),
             cfg_.seconds > 0 || cfg_.practice ? 0 : laps_, p.place, (int)racers_.size(),
             raceTime_, p.lap > 0 ? raceTime_ - p.lapStart : 0.0f, p.bestLap, p.lastLap, cd, (int)state_,
             p.finished ? "true" : "false", players_[pl].camMode, waiting ? "true" : "false",
             cfg_.seconds > 0 ? std::max(0.0f, cfg_.seconds - raceTime_) : -1.0f, cfg_.practice ? "true" : "false",
             p.car.rpm, p.car.spec.topSpeed * 3.6f);
    // Map: the player's car (position, heading), the other cars, the next and the missed checkpoints.
    std::string out(buf, strlen(buf) - 1);
    V3 f = p.car.R.col(1);
    snprintf(buf, sizeof buf, ",\"x\":%.2f,\"y\":%.2f,\"hd\":%.3f,\"next\":%d,\"missed\":[", p.car.pos.x,
             p.car.pos.y, std::atan2(f.y, f.x), p.nextCp);
    out += buf;
    std::vector<int> missed = missedCheckpoints(p);
    for (size_t i = 0; i < missed.size(); i++) out += (i ? "," : "") + std::to_string(missed[i]);
    // Passed this lap: the gates before the next one (all but the line once the lap is done), and the ones
    // crossed early after a cut. Before the first crossing of the start line nothing counts.
    out += "],\"passed\":[";
    const int ncp = (int)meta_.checkpoints.size();
    bool firstPassed = true;
    for (int g = 0; g < ncp && p.lap > 0; g++) {
        bool passed = p.nextCp == 0 ? g > 0 : g < p.nextCp;
        if (g < (int)p.cutPassed.size() && p.cutPassed[g]) passed = true;
        if (!passed) continue;
        out += (firstPassed ? "" : ",") + std::to_string(g);
        firstPassed = false;
    }
    out += "],\"cars\":[";
    bool first = true;
    for (const Racer& r : racers_) {
        if (&r == &p || r.gone || r.hidden) continue;
        snprintf(buf, sizeof buf, "%s[%.2f,%.2f]", first ? "" : ",", r.car.pos.x, r.car.pos.y);
        out += buf;
        first = false;
    }
    return out + "]}";
}

std::string Game::mapJson() const {
    if (meta_.aiLines.empty()) return "{}";
    const Path& line = meta_.aiLines[0];
    std::string out = "{\"line\":[";
    char buf[96];
    const int n = (int)line.pts.size(), step = std::max(1, n / 300);
    for (int i = 0; i < n; i += step) {
        snprintf(buf, sizeof buf, "%s[%.2f,%.2f]", i ? "," : "", line.pts[i].x, line.pts[i].y);
        out += buf;
    }
    out += "],\"cps\":[";
    for (size_t i = 0; i < meta_.checkpoints.size(); i++) {
        const Gate& g = meta_.checkpoints[i];
        snprintf(buf, sizeof buf, "%s[%.2f,%.2f,%.2f,%.2f]", i ? "," : "", g.a.x, g.a.y, g.b.x, g.b.y);
        out += buf;
    }
    return out + "]}";
}

std::string Game::resultsJson() const {
    std::string out = "[";
    std::vector<const Racer*> rs;
    for (auto& r : racers_) rs.push_back(&r);
    std::sort(rs.begin(), rs.end(), [](const Racer* a, const Racer* b) { return a->place < b->place; });
    for (size_t i = 0; i < rs.size(); i++) {
        const Racer& r = *rs[i];
        char b[320];
        snprintf(b, sizeof b,
                 "%s{\"i\":%d,\"player\":%s,\"human\":%d,\"remote\":%s,\"gone\":%s,\"place\":%d,\"finished\":%s,"
                 "\"time\":%.2f,\"best\":%.2f,\"lap\":%d,\"cls\":\"%s\"}",
                 i ? "," : "", (int)(rs[i] - racers_.data()), r.player >= 0 ? "true" : "false", r.player,
                 r.remote ? "true" : "false", r.gone ? "true" : "false", r.place, r.finished ? "true" : "false",
                 r.finishTime, r.bestLap, std::max(0, r.lap - 1), r.cls.c_str());
        out += b;
    }
    return out + "]";
}

void Game::clearance(int racer, float& body, float& wheel) const {
    body = wheel = 1e9f;
    const Racer& r = racers_[racer];
    const CarModel& cm = *carModels_[racer];
    auto below = [&](const V3& p) {
        Hit h;
        if (!world_.raycast(p + V3(0, 0, 0.3f), V3(0, 0, -1), 0.6f, h) || h.n.z < 0.5f) return 1e9f;
        return h.t - 0.3f;  // >0: p above ground
    };
    const V3& lo = cm.bodyLo;
    const V3& hi = cm.bodyHi;
    M4 bm = r.car.bodyMatrix();
    for (int i = 0; i < 4; i++) {
        V3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, lo.z);
        body = std::min(body, below(bm.point(c)));
    }
    for (int w = 0; w < 4 && w < (int)r.car.wheels.size(); w++) {
        if (!cm.wheel[w]) continue;
        V3 hub = bm.point(cm.wheelCentre[w] + V3(0, 0, r.car.wheels[w].comp));
        wheel = std::min(wheel, below(hub - r.car.R.col(2) * r.car.wheels[w].radius));
    }
}

std::string Game::screenJson(bool remoteOnly) const {
    std::string out = "[";
    const float* m = viewProj0_.m;
    for (size_t i = 0; i < racers_.size(); i++) {
        const Racer& r = racers_[i];
        if (r.gone || (remoteOnly && !r.remote) || (!players_.empty() && (int)i == players_[0].racer)) continue;
        V3 p = r.car.pos + V3(0, 0, r.car.boundRadius * 0.8f + 0.06f);  // just above the roof
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (cw < 0.05f) continue;  // behind the camera
        float x = (cx / cw + 1) * 0.5f, y = 1 - (cy / cw + 1) * 0.5f;
        if (x < -0.1f || x > 1.1f || y < -0.1f || y > 1.1f) continue;
        char b[96];
        snprintf(b, sizeof b, "%s[%d,%.4f,%.4f,%.2f]", out.size() > 1 ? "," : "", (int)i, vp0_[0] + x * vp0_[2],
                 vp0_[1] + y * vp0_[3], len(p - eye0_));
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
