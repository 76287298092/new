#include <jni.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "odyx_bridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "odyx_bridge", __VA_ARGS__)

using namespace std;

// typedef for external alvr core pose setter
using alvr_set_pose_t = void(*)(int64_t, float, float, float, float, float, float, float);

static alvr_set_pose_t g_alvr_set_pose = nullptr;

static void lookup_alvr_symbol() {
    if (g_alvr_set_pose) return;
    void* handle = dlopen("libalvr_client_core.so", RTLD_NOW);
    if (!handle) {
        // try default handle
        handle = dlopen(nullptr, RTLD_NOW);
    }
    if (handle) {
        void* sym = dlsym(handle, "alvr_client_set_external_pose");
        if (sym) {
            g_alvr_set_pose = reinterpret_cast<alvr_set_pose_t>(sym);
            LOGI("Found alvr_client_set_external_pose symbol");
        } else {
            LOGI("alvr_client_set_external_pose not found via dlsym");
        }
    } else {
        LOGI("dlopen failed to open libalvr_client_core.so: %s", dlerror());
    }
}

struct ImuSample {
    int64_t ts;
    float ax, ay, az;
    float gx, gy, gz;
};

struct CameraFrame {
    int64_t ts;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    int width;
    int height;
};

static std::mutex g_mutex;
static std::condition_variable g_cv;
static std::queue<ImuSample> g_imu_queue;
static std::queue<CameraFrame> g_cam_queue;
static std::atomic<bool> g_running(false);
static std::thread g_worker;

static void worker_thread() {
    LOGI("ODYX worker thread started");
    lookup_alvr_symbol();
    while (g_running.load()) {
        // simple wait
        std::unique_lock<std::mutex> lk(g_mutex);
        g_cv.wait_for(lk, std::chrono::milliseconds(20));

        // consume IMU samples (noop for now)
        while (!g_imu_queue.empty()) {
            ImuSample s = g_imu_queue.front(); g_imu_queue.pop();
            // In a full implementation, send to ODYX here
            // LOGI("Consuming IMU ts=%lld gx=%f gy=%f gz=%f", (long long)s.ts, s.gx, s.gy, s.gz);
        }

        // consume camera frames (noop for now)
        while (!g_cam_queue.empty()) {
            CameraFrame f = std::move(g_cam_queue.front()); g_cam_queue.pop();
            // In a full implementation, feed frame to ODYX here
            // LOGI("Consuming Camera frame ts=%lld w=%d h=%d", (long long)f.ts, f.width, f.height);
        }

        // simulate ODYX producing a pose periodically
        static uint64_t counter = 0;
        counter++;
        if (counter % 5 == 0) {
            int64_t now = (int64_t) (std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
            LOGI("ODYX produced dummy pose ts=%lld", (long long)now);
            if (g_alvr_set_pose) {
                g_alvr_set_pose(now, px, py, pz, qx, qy, qz, qw);
            } else {
                LOGI("alvr_client_set_external_pose not available, skipping send");
            }
        }
    }
    LOGI("ODYX worker thread stopping");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_viritualisres_phonevr_tracking_NativeOdYXBridge_nativeInit(JNIEnv* env, jclass clazz) {
    LOGI("nativeInit called");
    if (g_running.load()) return JNI_TRUE;
    g_running.store(true);
    g_worker = std::thread(worker_thread);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_tracking_NativeOdYXBridge_nativeShutdown(JNIEnv* env, jclass clazz) {
    LOGI("nativeShutdown called");
    if (!g_running.load()) return;
    g_running.store(false);
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_tracking_NativeOdYXBridge_nativeFeedImuSample(JNIEnv* env, jclass clazz,
    jlong timestampNs, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz) {
    ImuSample s;
    s.ts = (int64_t) timestampNs;
    s.ax = ax; s.ay = ay; s.az = az;
    s.gx = gx; s.gy = gy; s.gz = gz;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_imu_queue.push(s);
    }
    g_cv.notify_all();
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_tracking_NativeOdYXBridge_nativeFeedCameraFrame(JNIEnv* env, jclass clazz,
    jlong timestampNs, jbyteArray yArr, jbyteArray uArr, jbyteArray vArr, jint width, jint height) {
    CameraFrame f;
    f.ts = (int64_t) timestampNs;
    f.width = width; f.height = height;
    jsize ylen = env->GetArrayLength(yArr);
    jsize ulen = env->GetArrayLength(uArr);
    jsize vlen = env->GetArrayLength(vArr);
    f.y.resize(ylen);
    f.u.resize(ulen);
    f.v.resize(vlen);
    env->GetByteArrayRegion(yArr, 0, ylen, reinterpret_cast<jbyte*>(f.y.data()));
    env->GetByteArrayRegion(uArr, 0, ulen, reinterpret_cast<jbyte*>(f.u.data()));
    env->GetByteArrayRegion(vArr, 0, vlen, reinterpret_cast<jbyte*>(f.v.data()));
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_cam_queue.push(std::move(f));
    }
    g_cv.notify_all();
}

// optional hook to enable/disable pose callback from native (not implemented)
extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_tracking_NativeOdYXBridge_nativeSetPoseCallbackEnabled(JNIEnv* env, jclass clazz, jboolean enabled) {
    LOGI("nativeSetPoseCallbackEnabled: %d", enabled);
}
