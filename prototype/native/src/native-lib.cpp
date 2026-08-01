#include <jni.h>
#include <string>
#include <memory>
#include <mutex>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <android/log.h>

#include "include/tracker.h"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "alvr_vio", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "alvr_vio", __VA_ARGS__))

static std::unique_ptr<Tracker> g_tracker;
static std::mutex g_mutex;
static int g_sock = -1;
static struct sockaddr_in g_server_addr;

extern "C" JNIEXPORT void JNICALL
Java_com_example_alvrvio_NativeBridge_nativeInitSender(JNIEnv* env, jclass, jstring host, jint port){
    const char* chost = env->GetStringUTFChars(host, nullptr);
    LOGI("nativeInitSender host=%s port=%d", chost, port);
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(g_sock < 0){ LOGE("socket failed"); env->ReleaseStringUTFChars(host, chost); return; }
    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, chost, &g_server_addr.sin_addr);
    env->ReleaseStringUTFChars(host, chost);
}

static void send_pose_udp(const Pose& p){
    if(g_sock < 0) return;
    // packet: u64 ts, float px,py,pz, float qw,qx,qy,qz
    uint8_t buf[8 + 7 * 4];
    uint64_t ts = p.timestamp_ns;
    memcpy(buf, &ts, 8);
    memcpy(buf + 8, &p.px, 4);
    memcpy(buf + 12, &p.py, 4);
    memcpy(buf + 16, &p.pz, 4);
    memcpy(buf + 20, &p.qw, 4);
    memcpy(buf + 24, &p.qx, 4);
    memcpy(buf + 28, &p.qy, 4);
    memcpy(buf + 32, &p.qz, 4);
    sendto(g_sock, buf, sizeof(buf), 0, (struct sockaddr*)&g_server_addr, sizeof(g_server_addr));
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_alvrvio_NativeBridge_nativeStartTracking(JNIEnv* env, jclass){
    std::lock_guard<std::mutex> lock(g_mutex);
    if(!g_tracker){
        g_tracker = std::make_unique<Tracker>();
        g_tracker->setPoseCallback([](const Pose& p){ send_pose_udp(p); });
    }
    g_tracker->start();
    LOGI("nativeStartTracking");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_alvrvio_NativeBridge_nativeStopTracking(JNIEnv* env, jclass){
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_tracker) g_tracker->stop();
    LOGI("nativeStopTracking");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_alvrvio_NativeBridge_nativePushImu(JNIEnv* env, jclass, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz, jlong timestamp){
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_tracker){
        g_tracker->pushImu(ax, ay, az, gx, gy, gz, (uint64_t)timestamp);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_alvrvio_NativeBridge_nativePushCameraFrame(JNIEnv* env, jclass, jbyteArray data, jlong timestamp, jint width, jint height){
    jbyte* buf = env->GetByteArrayElements(data, nullptr);
    jsize len = env->GetArrayLength(data);
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_tracker){
        g_tracker->pushCameraFrame((const uint8_t*)buf, (size_t)len, (uint64_t)timestamp, width, height);
    }
    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
}
