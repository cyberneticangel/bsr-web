// Platform-independent game: track/car loading, race logic, camera and drawing.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "formats.h"
#include "netsync.h"
#include "physics.h"
#include "race.h"
#include "render.h"

namespace bsr {

struct Input {
    float steer = 0, throttle = 0, brake = 0;  // steer -1..1, right positive
    bool handbrake = false;
};

struct Callbacks {
    // player: local player index (split-screen). Per-player events are named "<event>:<player>".
    void (*audio)(int player, float rpm, float throttle, float skid, float impact, float speed) = nullptr;
    void (*event)(const char* name) = nullptr;
};

class Game {
public:
    enum State { Idle, Countdown, Racing, Finished };
    static const int kMaxLocal = 2;

    bool init();
    // roles: one character per racer, in grid order (racer 0 starts at the back):
    //   '0'/'1' local human player, 'a' AI simulated here, 'r' remote car driven by network snapshots.
    // Empty means one local player plus `opponents` AI cars. skins are assigned to racers in order (cycling).
    bool start(const std::string& track, const std::string& carClass, const std::vector<std::string>& skins,
               int opponents, int laps, float topSpeedKmh, const std::string& weather, const std::string& roles = "");
    void stop();
    void update(float dt, const Input* in, int numInputs);  // in[p] drives local player p
    void update(float dt, const Input& in) { update(dt, &in, 1); }
    void render(int w, int h);  // one viewport per local player, stacked top to bottom
    void cycleCamera(int p = 0);
    void setCamera(int m);
    void resetPlayer(int p = 0);
    std::string hudJson(int p = 0) const;
    std::string resultsJson() const;

    // Networking. now: local clock in seconds.
    bool packState(int racer, double now, float* out) const;  // false unless the racer is simulated here
    void pushSnapshot(int racer, const float* snap, double now);
    void removeRacer(int racer);
    void setNow(double now) { now_ = now; }

    Callbacks cb;
    bool paused = false;
    bool waiting = false;    // grid is set but the countdown is held (online: waiting for every player to load)
    bool autopilot = false;  // player car driven by AI (tests / after finishing)
    bool debugLines = false;
    State state() const { return state_; }
    int localPlayers() const { return (int)players_.size(); }
    float raceTime() const { return raceTime_; }
    const std::vector<Racer>& racers() const { return racers_; }
    const TrackMeta& meta() const { return meta_; }

private:
    struct CarModel {
        Model model;
        std::vector<Part*> parts;
        Part* wheel[4] = {};
        V3 wheelCentre[4];
    };
    void buildCarModel(CarModel& cm, const FSO& fso, const std::map<std::string, std::string>& ov);
    struct View {
        int racer = 0;
        int camMode = 0;
        V3 camPos;
        bool camInit = false;
    };
    void placeOnLine(Racer& r);
    void updateCamera(View& v, float dt, int x, int y, int w, int h);
    void draw();
    void stepPhysics(float dt);
    void updateRace(float dt);
    void emit(const char* e) { if (cb.event) cb.event(e); }

    Renderer gfx_;
    std::string trackName_, weatherName_;
    FSO trackFso_;
    Model track_;
    FST fst_;
    CollisionWorld world_;
    TrackMeta meta_;
    std::vector<std::unique_ptr<CarModel>> carModels_;
    std::vector<Racer> racers_;
    std::vector<SnapshotBuffer> snaps_;  // per racer; used by remote racers only
    std::vector<View> players_;          // local human players, in player order
    int laps_ = 3;
    float raceTime_ = 0, countdown_ = 0, accum_ = 0, camDt_ = 0;
    double now_ = 0;
    State state_ = Idle;
    int defaultCam_ = 0;
    int lastCountdownSec_ = -1;
};

}  // namespace bsr
