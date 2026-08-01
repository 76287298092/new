#include "alvr_client_core.h"
#include <android/log.h>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "alvr_pose_bridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "alvr_pose_bridge", __VA_ARGS__)

extern "C" void alvr_client_set_external_pose(int64_t timestamp_ns,
                                               float px, float py, float pz,
                                               float qx, float qy, float qz, float qw) {
    // Build a simple AlvrViewParams and AlvrDeviceMotion and send as tracking
    AlvrViewParams view = {};
    // set pose
    view.pose.position[0] = px;
    view.pose.position[1] = py;
    view.pose.position[2] = pz;
    view.pose.orientation[0] = qx;
    view.pose.orientation[1] = qy;
    view.pose.orientation[2] = qz;
    view.pose.orientation[3] = qw;

    AlvrDeviceMotion head = {};
    head.pose = view.pose;
    head.device_id = alvr_path_string_to_id("/user/head");

    // send tracking to the alvr core
    LOGI("alvr_client_set_external_pose ts=%lld pos=(%f,%f,%f) quat=(%f,%f,%f,%f)",
         (long long)timestamp_ns, px, py, pz, qx, qy, qz, qw);

    // We pass the view array containing one view
    AlvrViewParams views[1];
    views[0] = view;

    alvr_send_tracking((uint64_t)timestamp_ns, views, &head, 1, nullptr, nullptr);
}
