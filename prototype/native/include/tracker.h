#ifndef ALVR_VIO_TRACKER_H
#define ALVR_VIO_TRACKER_H

#include <cstdint>
#include <functional>

struct Pose {
    uint64_t timestamp_ns;
    float px, py, pz;
    float qw, qx, qy, qz;
};

class Tracker {
public:
    using PoseCallback = std::function<void(const Pose&)>;

    Tracker();
    ~Tracker();

    void start();
    void stop();

    // Push a camera frame (YUV420 byte buffer). For prototype we only pass pointer/size.
    void pushCameraFrame(const uint8_t* data, size_t size, uint64_t timestamp_ns, int width, int height);

    // Push IMU sample (accelerometer m/s^2, gyro rad/s)
    void pushImu(float ax, float ay, float az, float gx, float gy, float gz, uint64_t timestamp_ns);

    void setPoseCallback(PoseCallback cb);

private:
    PoseCallback callback_;
    bool running_;
    // internal state (dummy)
    float px_, py_, pz_;
    float qw_, qx_, qy_, qz_;
};

#endif // ALVR_VIO_TRACKER_H
