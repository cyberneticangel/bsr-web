// Platform-independent game: track/car loading, race logic, camera and drawing.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "formats.h"
#include "physics.h"
#include "race.h"
#include "render.h"

namespace bsr {

struct Input {
    float steer = 0, throttle = 0, brake = 0;  // steer -1..1, right positive
    bool handbrake = false;
};

struct Callbacks {
    void (*hud)(const char* json) = nullptr;
    void (*audio)(float rpm, float throttle, float skid, float impact, float speed) = nullptr;
    void (*event)(const char* name) = nullptr;
};

class Game {
public:
    enum State { Idle, Countdown, Racing, Finished };

    bool init();
    bool start(const std::string& track, const std::string& carClass, const std::vector<std::string>& skins,
               int opponents, int laps, float topSpeedKmh, const std::string& weather);
    void stop();
    void update(float dt, const Input& in);
    void render(int w, int h);
    void cycleCamera() { camMode_ = (camMode_ + 1) % 3; camInit_ = false; }
    void setCamera(int m) { camMode_ = m % 3; camInit_ = false; }
    void resetPlayer();
    std::string hudJson() const;
    std::string resultsJson() const;

    Callbacks cb;
    bool paused = false;
    bool autopilot = false;  // player car driven by AI (tests / after finishing)
    bool debugLines = false;
    State state() const { return state_; }
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
    void placeOnLine(Racer& r);
    void updateCamera(float dt, int w, int h);
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
    int laps_ = 3;
    float raceTime_ = 0, countdown_ = 0, accum_ = 0, camDt_ = 0;
    State state_ = Idle;
    int camMode_ = 0;
    V3 camPos_;
    bool camInit_ = false;
    int lastCountdownSec_ = -1;
};

}  // namespace bsr
