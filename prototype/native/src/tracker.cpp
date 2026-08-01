#include "tracker.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

Tracker::Tracker()
: running_(false), px_(0), py_(0), pz_(0), qw_(1), qx_(0), qy_(0), qz_(0) {}

Tracker::~Tracker(){ stop(); }

void Tracker::start(){ running_ = true; }
void Tracker::stop(){ running_ = false; }

void Tracker::pushCameraFrame(const uint8_t* data, size_t size, uint64_t timestamp_ns, int width, int height){
    // Prototype: do nothing for visuals
    (void)data; (void)size; (void)timestamp_ns; (void)width; (void)height;
}

void Tracker::pushImu(float ax, float ay, float az, float gx, float gy, float gz, uint64_t timestamp_ns){
    (void)timestamp_ns;
    // Very small dead-reckoning: integrate gyro to rotate quaternion slightly
    // This is only placeholder logic for a running prototype.
    const float dt = 0.005f; // assume small dt
    // Simple position drift using accelerometer (not realistic)
    px_ += ax * dt * dt * 0.5f;
    py_ += ay * dt * dt * 0.5f;
    pz_ += az * dt * dt * 0.5f;

    if(callback_){
        Pose p;
        p.timestamp_ns = timestamp_ns;
        p.px = px_; p.py = py_; p.pz = pz_;
        p.qw = qw_; p.qx = qx_; p.qy = qy_; p.qz = qz_;
        callback_(p);
    }
}

void Tracker::setPoseCallback(PoseCallback cb){ callback_ = cb; }
