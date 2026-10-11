// Platform-independent game: track/car loading, race logic, camera and drawing.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "effects.h"
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
    // player: local player index (split-screen). Per-player events are named "<event>:<player>[:<value>]".
    void (*audio)(int player, float rpm, float throttle, float skid, float impact, float speed) = nullptr;
    void (*event)(const char* name) = nullptr;
    // Once per rendered frame: player 0's camera (positional audio). eye, forward and up, world space.
    void (*listener)(const V3& eye, const V3& fwd, const V3& up) = nullptr;
};

struct CarEntry {
    std::string cls;          // car class: cars/car_<cls>.fso
    std::string skin;         // body texture
    float topSpeedKmh = 50;
    char role = 'a';          // '0'/'1' local human player, 'a' AI simulated here, 'r' remote (network snapshots)
    float skill = 0;          // AI pace 0..1 (0: by grid slot)
};

struct RaceConfig {
    std::string track, weather = "sunny_bluesky_summer";  // weather.bin preset
    std::string weatherCat;      // menu category (sunny, cloudy, sunset, wet, rain): the track's texture swaps
    std::vector<CarEntry> cars;  // grid order: cars[0] starts at the back
    int laps = 3;                // lap race (ignored when seconds > 0)
    float seconds = 0;           // timed race: once time is up, each car finishes at its next line crossing
    bool practice = false;       // no finish: laps run until the player quits
    int ghost = 0;               // ghost car replaying the player's 1 best lap, 2 last lap (0 off)
};
// Parses "track=mach;weather=...;laps=3;time=0;practice=0;ghost=0;cars=cls,skin,top,role[,skill]|..."
bool parseRaceConfig(const std::string& s, RaceConfig& out);

class Game {
public:
    enum State { Idle, Countdown, Racing, Finished, Podium };
    static const int kMaxLocal = 2;
    // Camera modes, the original's player views (Tracks/<track>_cams.bin), cycled with C.
    enum Cam { CamFollow, CamFollow2, CamFollow3, CamBumper, CamTower, CamTV, kCamModes };

    bool init();
    bool start(const RaceConfig& cfg);
    // Single class, one local player plus `opponents` AI cars (tests). skins are assigned in order, cycling.
    bool start(const std::string& track, const std::string& carClass, const std::vector<std::string>& skins,
               int opponents, int laps, float topSpeedKmh, const std::string& weather);
    void stop();
    // Prize ceremony: the top three on the track's podium with their cups (top eight on the grid in front),
    // seen from an orbiting camera. Racing stops; stop() or start() ends it.
    bool startPodium();
    void update(float dt, const Input* in, int numInputs);  // in[p] drives local player p
    void update(float dt, const Input& in) { update(dt, &in, 1); }
    void render(int w, int h);  // one viewport per local player, stacked top to bottom
    void cycleCamera(int p = 0);
    void setCamera(int m);
    void resetPlayer(int p = 0);
    std::string hudJson(int p = 0) const;
    // Renders one car (class, skin texture) on a transparent background for the menu. rgba: w*h*4, rows bottom-up.
    bool renderPreview(const std::string& cls, const std::string& skin, int w, int h, std::vector<uint8_t>& rgba);
    std::string resultsJson() const;
    // Where the cars are on screen in player 0's view (for name tags): JSON [[racer, x, y, distance], ...] with
    // x, y in 0..1 of the canvas (y down), for cars in front of the camera. remoteOnly: only network cars.
    std::string screenJson(bool remoteOnly) const;

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
    Effects& effects() { return fx_; }
    State state() const { return state_; }
    bool raining() const { return rain_; }
    bool wet() const { return wet_; }
    int localPlayers() const { return (int)players_.size(); }
    float raceTime() const { return raceTime_; }
    const std::vector<Racer>& racers() const { return racers_; }
    const TrackMeta& meta() const { return meta_; }
    const CollisionWorld& world() const { return world_; }
    // Trackside loudspeakers (centres of the track's speaker models), for localized music.
    const std::vector<V3>& speakers() const { return speakers_; }
    // Diagnostics: lowest height of the drawn body's bottom corners (body) and wheel bottoms (wheel) above the
    // ground right under them; negative = drawn inside the ground.
    void clearance(int racer, float& body, float& wheel) const;

private:
    struct CarModel {
        Model model;
        std::vector<Part*> parts;
        Part* wheel[4] = {};
        V3 wheelCentre[4];
        V3 bodyLo, bodyHi;  // model-space bounds of the solid body parts (no wheels, shadow or helpers)
    };
    void buildCarModel(CarModel& cm, const FSO& fso, const std::map<std::string, std::string>& ov);
    struct View {
        int racer = 0;
        int camMode = 0;
        int tvHint = 0;  // racing line index near the car (TV camera choice)
        V3 camPos;
        bool camInit = false;
    };
    void placeOnLine(Racer& r);
    void updateCamera(View& v, float dt, int x, int y, int w, int h);
    void draw();
    void stepPhysics(float dt);
    void updateRace(float dt);
    void emit(const char* e) { if (cb.event) cb.event(e); }
    void emitf(const char* fmt, ...);
    void finishLap(Racer& r);
    void checkpointWarnings(Racer& r, float dt);
    void updateGhost(float dt);
    CarModel* carModel(const std::string& cls, const std::string& skin);

    Renderer gfx_;
    Effects fx_;
    V3 camEye_, camRight_, camUp_;  // the view being drawn
    M4 viewProj0_;                  // player 0's last camera
    V3 eye0_;
    float vp0_[4] = {0, 0, 1, 1};    // and its viewport as fractions of the canvas: x, y (from top), w, h
    std::string trackName_, weatherName_;
    bool rain_ = false, wet_ = false, thunder_ = false;
    FSO trackFso_;
    Model track_;
    Model prizes_;  // prizes_track.fso: gold, silver and bronze cups (parts "1", "2", "3")
    FST fst_;
    CollisionWorld world_;
    TrackMeta meta_;
    std::vector<std::unique_ptr<CarModel>> carModels_;
    std::vector<Racer> racers_;
    std::vector<SnapshotBuffer> snaps_;  // per racer; used by remote racers only
    std::vector<View> players_;          // local human players, in player order
    RaceConfig cfg_;
    std::vector<V3> speakers_;
    std::vector<TrackCam> cams_;
    const TrackCam* findCam(const std::string& name) const;
    std::map<std::string, FSO> carFsos_;  // per class, for this race
    int laps_ = 3;
    bool timeUp_ = false;
    int minutesLeftSaid_ = 0;
    // Ghost: the player's laps recorded as snapshots every kGhostStep seconds of lap time.
    struct GhostLap { std::vector<std::vector<float>> frames; float time = 0; };
    GhostLap ghostCur_, ghostBest_, ghostLast_;
    std::string ghostKey_;  // track|class the stored laps belong to
    std::unique_ptr<CarModel> ghostModel_;
    Racer ghost_;
    bool ghostVisible_ = false;
    float raceTime_ = 0, countdown_ = 0, accum_ = 0, camDt_ = 0;
    double now_ = 0;
    State state_ = Idle;
    float podiumTime_ = 0;
    void podiumCamera(int x, int y, int w, int h);
    int defaultCam_ = 0;
    int lastCountdownSec_ = -1;
};

}  // namespace bsr
