#include "alvr_client_core.h"
#include <GLES3/gl3.h>
#include <algorithm>
#include <android/log.h>
#include <deque>
#include <jni.h>
#include <map>
#include <thread>
#include <unistd.h>
#include <vector>

#include "nlohmann/json.hpp"
#include "utils.h"

// ==========================================================================
// ODYX 6DoF + PhoneVR :: 普通手机 → PC VR 显示器
//
// 改动说明：
//   1. 移除 CardboardHeadTracker (3DoF) → 替换为 ODYX VIO (6DoF)
//   2. 保留 CardboardLensDistortion + DistortionRenderer (镜片畸变仍需)
//   3. 追踪数据通过 JNI 从 OdyxEngineWrapper.getPose() 获取
//   4. odyx_jni.cpp (ODYX native) 与 OdyxEngineWrapper.kt 管理底层 SLAM
// ==========================================================================

using namespace nlohmann;

uint64_t HEAD_ID = alvr_path_string_to_id("/user/head");

const float FLOOR_HEIGHT = 1.5;
const int MAXIMUM_TRACKING_FRAMES = 360;

// ---- JNI 辅助函数 --------------------------------------------------------

JavaVM *g_vm = nullptr;

JNIEnv *getEnv() {
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

// ---- ODYX 6DoF 追踪接口 (替换 CardboardHeadTracker) ----------------------

struct OdyxBridge {
    jclass wrapperClass = nullptr;
    jmethodID getPoseMethod = nullptr;
    jmethodID initMethod = nullptr;
    bool initialized = false;
};

OdyxBridge ODYX = {};

/// 初始化 ODYX JNI 桥接
void initOdyx(JNIEnv *env, jobject context) {
    if (ODYX.initialized) return;

    jclass cls = env->FindClass("com/alvr/odyx/OdyxEngineWrapper");
    if (cls == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "OdyxVR",
            "OdyxEngineWrapper class not found!");
        return;
    }
    ODYX.wrapperClass = (jclass)env->NewGlobalRef(cls);

    ODYX.initMethod = env->GetStaticMethodID(
        cls, "init", "(Landroid/content/Context;)V");
    ODYX.getPoseMethod = env->GetStaticMethodID(
        cls, "getPose", "()[F");

    if (ODYX.initMethod == nullptr || ODYX.getPoseMethod == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "OdyxVR",
            "OdyxEngineWrapper methods not found!");
        return;
    }

    // 启动 ODYX 引擎（相机 + IMU + 位姿轮询线程）
    env->CallStaticVoidMethod(cls, ODYX.initMethod, context);
    ODYX.initialized = true;

    __android_log_print(ANDROID_LOG_INFO, "OdyxVR",
        "ODYX 6DoF engine initialized");
}

/// 通过 JNI 获取 ODYX 6DoF 位姿
/// 返回 {px, py, pz, qx, qy, qz, qw}，坐标系 ENU (Z-up)
AlvrPose getOdyxPose() {
    AlvrPose pose = {};
    // 默认：单位四元数 + 原点
    pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    pose.position = {0.0f, 0.0f, 0.0f};

    if (!ODYX.initialized) return pose;

    JNIEnv *env = getEnv();
    if (env == nullptr) return pose;

    jfloatArray arr = (jfloatArray)env->CallStaticObjectMethod(
        ODYX.wrapperClass, ODYX.getPoseMethod);

    if (arr == nullptr) return pose;

    jfloat *elems = env->GetFloatArrayElements(arr, nullptr);
    if (elems != nullptr && env->GetArrayLength(arr) >= 7) {
        // OdyxEngineWrapper.getPose() 返回 [x, y, z, qx, qy, qz, qw]
        // 坐标系为 ENU (Z-up)
        // OpenGL/SteamVR 使用 Y-up，需要绕 X 轴旋转 -90°
        float x = elems[0];
        float y = elems[1];
        float z = elems[2];
        float qx = elems[3];
        float qy = elems[4];
        float qz = elems[5];
        float qw = elems[6];

        // 坐标系转换: ENU (Z-up) → OpenGL/SteamVR (Y-up)
        // 绕 X 轴旋转 -90°:
        //   位置: (x, y, z) → (x, z, -y)
        //   四元数: q_new = rot * q
        //   rot = cos(-45°) + sin(-45°)*i = (0.707, -0.707, 0, 0)
        //   实现: 直接用四元数乘法
        const float sqrt2_2 = 0.70710678f;
        // 绕 X 轴旋转 -90° 的四元数: q = cos(-45°) + i*sin(-45°)
        // 即 qw=0.707, qx=-0.707, qy=0, qz=0
        // q_new = rot * q_enu
        float rw = sqrt2_2, rx = -sqrt2_2;
        pose.position = {x, z, -y};  // 坐标轴交换
        pose.orientation = {
            rw*qx + rx*qw,            // qx_new = rw*qx + rx*qw
            rw*qy - rx*qz,            // qy_new = rw*qy - rx*qz
            rw*qz + rx*qy,            // qz_new = rw*qz + rx*qy
            rw*qw - rx*qx             // qw_new = rw*qw - rx*qx
        };
    }
    env->ReleaseFloatArrayElements(arr, elems, JNI_ABORT);

    return pose;
}

// ---- Cardboard 镜片畸变渲染 (保留) ---------------------------------------

#include "cardboard.h"   // CardboardLensDistortion + DistortionRenderer

struct NativeContext {
    JavaVM *javaVm = nullptr;
    jobject javaContext = nullptr;

    // 注意: CardboardHeadTracker 已移除，替换为 ODYX 6DoF

    CardboardLensDistortion *lensDistortion = nullptr;
    CardboardDistortionRenderer *distortionRenderer = nullptr;

    int screenWidth = 0;
    int screenHeight = 0;

    bool renderingParamsChanged = true;
    bool glContextRecreated = false;

    bool running = false;
    bool streaming = false;
    std::thread inputThread;

    // 双眼纹理
    GLuint lobbyTextures[2] = {0, 0};
    GLuint streamTextures[2] = {0, 0};

    float eyeOffsets[2] = {0.0, 0.0};
    AlvrFov fovArr[2] = {};
    AlvrViewParams viewParams[2] = {};
    AlvrDeviceMotion deviceMotion = {};

    NativeContext() {
        memset(&fovArr, 0, (sizeof(fovArr)) / sizeof(int));
        memset(&viewParams, 0, (sizeof(viewParams)) / sizeof(int));
        memset(&deviceMotion, 0, (sizeof(deviceMotion)) / sizeof(int));
    }
};

NativeContext CTX = {};

int64_t GetBootTimeNano() {
    struct timespec res = {};
    clock_gettime(CLOCK_BOOTTIME, &res);
    return (res.tv_sec * 1e9) + res.tv_nsec;
}

// 逆单位四元数
AlvrQuat inverseQuat(AlvrQuat q) { return {-q.x, -q.y, -q.z, q.w}; }

void cross(float a[3], float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void quatVecMultiply(AlvrQuat q, float v[3], float out[3]) {
    float rv[3], rrv[3];
    float r[3] = {q.x, q.y, q.z};
    cross(r, v, rv);
    cross(r, rv, rrv);
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] + 2 * (q.w * rv[i] + rrv[i]);
    }
}

void offsetPosWithQuat(AlvrQuat q, float offset[3], float outPos[3]) {
    float rotatedOffset[3];
    quatVecMultiply(q, offset, rotatedOffset);

    outPos[0] -= rotatedOffset[0];
    outPos[1] -= rotatedOffset[1] - FLOOR_HEIGHT;
    outPos[2] -= rotatedOffset[2];
}

AlvrFov getFov(CardboardEye eye) {
    float f[4];
    CardboardLensDistortion_getFieldOfView(CTX.lensDistortion, eye, f);

    AlvrFov fov = {};
    fov.left = -f[0];
    fov.right = f[1];
    fov.up = f[3];
    fov.down = -f[2];
    return fov;
}

// ---- 6DoF 追踪更新 (核心改动) --------------------------------------------

void updateViewConfigs(uint64_t targetTimestampNs = 0) {
    if (!targetTimestampNs)
        targetTimestampNs = GetBootTimeNano() + alvr_get_head_prediction_offset_ns();

    // 从 ODYX 获取 6DoF 位姿 (位置 + 旋转)
    AlvrPose headPose = getOdyxPose();

    CTX.deviceMotion.device_id = HEAD_ID;
    CTX.deviceMotion.pose = headPose;

    float headToEye[3] = {CTX.eyeOffsets[kLeft], 0.0, 0.0};

    CTX.viewParams[kLeft].pose = headPose;
    offsetPosWithQuat(headPose.orientation, headToEye, CTX.viewParams[kLeft].pose.position);
    CTX.viewParams[kLeft].fov = CTX.fovArr[kLeft];

    headToEye[0] = CTX.eyeOffsets[kRight];
    CTX.viewParams[kRight].pose = headPose;
    offsetPosWithQuat(headPose.orientation, headToEye, CTX.viewParams[kRight].pose.position);
    CTX.viewParams[kRight].fov = CTX.fovArr[kRight];
}

void inputThread() {
    auto deadline = std::chrono::steady_clock::now();
    __android_log_print(ANDROID_LOG_INFO, "OdyxVR", "inputThread started");

    while (CTX.streaming) {
        auto targetTimestampNs = GetBootTimeNano() + alvr_get_head_prediction_offset_ns();
        updateViewConfigs(targetTimestampNs);

        alvr_send_tracking(
            targetTimestampNs, CTX.viewParams, &CTX.deviceMotion, 1, nullptr, nullptr);

        deadline += std::chrono::nanoseconds((uint64_t)(1e9 / 60.f / 3));
        std::this_thread::sleep_until(deadline);
    }
}

// ---- JNI 入口 -----------------------------------------------------------

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    CTX.javaVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_initializeNative(
    JNIEnv *env, jobject obj, jint screenWidth, jint screenHeight,
    jfloat refreshRate) {

    CTX.javaContext = env->NewGlobalRef(obj);

    // ===== 新增: 初始化 ODYX 6DoF 引擎 =====
    initOdyx(env, obj);
    // ========================================

    uint32_t viewWidth = std::max(screenWidth, screenHeight) / 2;
    uint32_t viewHeight = std::min(screenWidth, screenHeight);

    alvr_initialize_android_context((void *)CTX.javaVm, (void *)CTX.javaContext);

    float refreshRatesBuffer[1] = {refreshRate};

    AlvrClientCapabilities caps = {};
    caps.default_view_height = viewHeight;
    caps.default_view_width = viewWidth;
    caps.external_decoder = false;
    caps.refresh_rates = refreshRatesBuffer;
    caps.refresh_rates_count = 1;
    caps.foveated_encoding = true;
    caps.encoder_high_profile = true;
    caps.encoder_10_bits = true;
    caps.encoder_av1 = true;

    alvr_initialize(caps);

    // 注意: 不再初始化 CardboardHeadTracker (3DoF → ODYX 6DoF)
    Cardboard_initializeAndroid(CTX.javaVm, CTX.javaContext);
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_destroyNative(JNIEnv *, jobject) {
    alvr_destroy_opengl();
    alvr_destroy();

    CardboardLensDistortion_destroy(CTX.lensDistortion);
    CTX.lensDistortion = nullptr;
    CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
    CTX.distortionRenderer = nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_resumeNative(JNIEnv *, jobject) {
    // 注意: 不再调用 CardboardHeadTracker_resume

    CTX.renderingParamsChanged = true;

    // Cardboard QR 码对 6DoF 非必需，但保留以获取镜片参数
    uint8_t *buffer;
    int size;
    CardboardQrCode_getSavedDeviceParams(&buffer, &size);
    if (size == 0) {
        // 如果首次使用，需要扫描 QR 码获取镜片参数
        // 可以跳过，使用默认参数
    }
    CardboardQrCode_destroy(buffer);

    CTX.running = true;
    alvr_resume();
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_pauseNative(JNIEnv *, jobject) {
    alvr_pause();

    if (CTX.running) {
        CTX.running = false;
    }
    // 注意: 不再调用 CardboardHeadTracker_pause
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_surfaceCreatedNative(JNIEnv *, jobject) {
    alvr_initialize_opengl();
    CTX.glContextRecreated = true;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_setScreenResolutionNative(
    JNIEnv *, jobject, jint width, jint height) {
    CTX.screenWidth = width;
    CTX.screenHeight = height;
    CTX.renderingParamsChanged = true;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_sendBatteryLevel(
    JNIEnv *, jobject, jfloat level, jboolean plugged) {
    alvr_send_battery(HEAD_ID, level, plugged);
}

// ---- 主渲染循环 ---------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_renderNative(JNIEnv *, jobject) {
    try {
        if (CTX.renderingParamsChanged) {
            __android_log_print(ANDROID_LOG_INFO, "OdyxVR",
                "renderingParamsChanged, processing new params");

            uint8_t *buffer;
            int size;
            CardboardQrCode_getSavedDeviceParams(&buffer, &size);

            if (size == 0) {
                // 没有 Cardboard QR 参数时使用默认值
                // 这里可以设置一个默认的 FOV 和 IPD
                CTX.fovArr[kLeft] = {-0.7f, 0.7f, 0.7f, -0.7f};
                CTX.fovArr[kRight] = {-0.7f, 0.7f, 0.7f, -0.7f};
                CTX.eyeOffsets[0] = -0.032f;  // 默认 IPD 32mm
                CTX.eyeOffsets[1] = 0.032f;

                CTX.renderingParamsChanged = false;
                CTX.glContextRecreated = false;
                return;
            }

            if (CTX.lensDistortion) {
                CardboardLensDistortion_destroy(CTX.lensDistortion);
                CTX.lensDistortion = nullptr;
            }
            CTX.lensDistortion =
                CardboardLensDistortion_create(buffer, size, CTX.screenWidth, CTX.screenHeight);

            CardboardQrCode_destroy(buffer);

            if (CTX.distortionRenderer) {
                CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
                CTX.distortionRenderer = nullptr;
            }
            const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
            CTX.distortionRenderer = CardboardOpenGlEs2DistortionRenderer_create(&config);

            for (int eye = 0; eye < 2; eye++) {
                CardboardMesh mesh;
                CardboardLensDistortion_getDistortionMesh(
                    CTX.lensDistortion, (CardboardEye)eye, &mesh);
                CardboardDistortionRenderer_setMesh(
                    CTX.distortionRenderer, &mesh, (CardboardEye)eye);

                float matrix[16] = {};
                CardboardLensDistortion_getEyeFromHeadMatrix(
                    CTX.lensDistortion, (CardboardEye)eye, matrix);
                CTX.eyeOffsets[eye] = matrix[12];
            }

            CTX.fovArr[kLeft] = getFov(kLeft);
            CTX.fovArr[kRight] = getFov(kRight);

            CTX.renderingParamsChanged = false;
            CTX.glContextRecreated = false;
        }

        // GL 上下文重建处理
        if (CTX.renderingParamsChanged && !CTX.glContextRecreated) {
            alvr_pause_opengl();
            GL(glDeleteTextures(2, CTX.lobbyTextures));
        }

        if (CTX.renderingParamsChanged || CTX.glContextRecreated) {
            GL(glGenTextures(2, CTX.lobbyTextures));

            for (auto &lobbyTexture : CTX.lobbyTextures) {
                GL(glBindTexture(GL_TEXTURE_2D, lobbyTexture));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                    CTX.screenWidth / 2, CTX.screenHeight, 0,
                    GL_RGB, GL_UNSIGNED_BYTE, nullptr));
            }

            const uint32_t *targetViews[2] = {
                (uint32_t *)&CTX.lobbyTextures[0],
                (uint32_t *)&CTX.lobbyTextures[1]};
            alvr_resume_opengl(CTX.screenWidth / 2, CTX.screenHeight, targetViews, 1, true);

            CTX.renderingParamsChanged = false;
            CTX.glContextRecreated = false;
        }

        // 事件循环
        AlvrEvent event;
        while (alvr_poll_event(&event)) {
            if (event.tag == ALVR_EVENT_HUD_MESSAGE_UPDATED) {
                auto message_length = alvr_hud_message(nullptr);
                auto message_buffer = std::vector<char>(message_length);
                alvr_hud_message(&message_buffer[0]);
                if (message_length > 0)
                    alvr_update_hud_message_opengl(&message_buffer[0]);
            }
            if (event.tag == ALVR_EVENT_STREAMING_STARTED) {
                __android_log_print(ANDROID_LOG_INFO, "OdyxVR",
                    "Streaming started, generating textures...");
                auto config = event.STREAMING_STARTED;

                auto settings_len = alvr_get_settings_json(nullptr);
                auto settings_buffer = std::vector<char>(settings_len);
                alvr_get_settings_json(&settings_buffer[0]);
                json settings_json = json::parse(&settings_buffer[0]);

                GL(glGenTextures(2, CTX.streamTextures));

                for (auto &streamTexture : CTX.streamTextures) {
                    GL(glBindTexture(GL_TEXTURE_2D, streamTexture));
                    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                    GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                        config.view_width, config.view_height, 0,
                        GL_RGB, GL_UNSIGNED_BYTE, nullptr));
                }

                CTX.fovArr[0] = getFov((CardboardEye)0);
                CTX.fovArr[1] = getFov((CardboardEye)1);

                auto leftIntHandle = (uint32_t)CTX.streamTextures[0];
                auto rightIntHandle = (uint32_t)CTX.streamTextures[1];
                const uint32_t *textureHandles[2] = {&leftIntHandle, &rightIntHandle};

                auto render_config = AlvrStreamConfig{};
                render_config.view_resolution_width = config.view_width;
                render_config.view_resolution_height = config.view_height;
                render_config.swapchain_textures = textureHandles;
                render_config.swapchain_length = 1;
                render_config.enable_foveation = false;

                if (!settings_json["video"].is_null() &&
                    !settings_json["video"]["foveated_encoding"].is_null() &&
                    !settings_json["video"]["foveated_encoding"].is_string()) {
                    render_config.enable_foveation = true;
                    render_config.foveation_center_size_x =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["center_size_x"];
                    render_config.foveation_center_size_y =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["center_size_y"];
                    render_config.foveation_center_shift_x =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["center_shift_x"];
                    render_config.foveation_center_shift_y =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["center_shift_y"];
                    render_config.foveation_edge_ratio_x =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["edge_ratio_x"];
                    render_config.foveation_edge_ratio_y =
                        settings_json["video"]["foveated_encoding"]["Enabled"]["edge_ratio_y"];
                }

                alvr_start_stream_opengl(render_config);

                CTX.streaming = true;
                CTX.inputThread = std::thread(inputThread);

            } else if (event.tag == ALVR_EVENT_STREAMING_STOPPED) {
                CTX.streaming = false;
                CTX.inputThread.join();
                GL(glDeleteTextures(2, CTX.streamTextures));
            }
        }

        // 渲染到双眼纹理
        CardboardEyeTextureDescription viewsDescs[2] = {};
        for (auto &viewsDesc : viewsDescs) {
            viewsDesc.left_u = 0.0;
            viewsDesc.right_u = 1.0;
            viewsDesc.top_v = 1.0;
            viewsDesc.bottom_v = 0.0;
        }

        if (CTX.streaming) {
            void *streamHardwareBuffer = nullptr;

            AlvrViewParams dummyViewParams;
            auto timestampNs = alvr_get_frame(&dummyViewParams, &streamHardwareBuffer);

            if (timestampNs == -1) {
                return;
            }

            uint32_t swapchainIndices[2] = {0, 0};
            alvr_render_stream_opengl(streamHardwareBuffer, swapchainIndices);
            alvr_report_submit(timestampNs, 0);

            viewsDescs[0].texture = CTX.streamTextures[0];
            viewsDescs[1].texture = CTX.streamTextures[1];
        } else {
            // Lobby 模式下用 ODYX 6DoF 渲染
            AlvrPose pose = getOdyxPose();

            AlvrViewInput viewInputs[2] = {};
            for (int eye = 0; eye < 2; eye++) {
                float headToEye[3] = {CTX.eyeOffsets[eye], 0.0, 0.0};
                offsetPosWithQuat(pose.orientation, headToEye,
                    viewInputs[eye].pose.position);

                viewInputs[eye].pose.orientation = pose.orientation;
                viewInputs[eye].fov = getFov((CardboardEye)eye);
                viewInputs[eye].swapchain_index = 0;
            }
            alvr_render_lobby_opengl(viewInputs);

            viewsDescs[0].texture = CTX.lobbyTextures[0];
            viewsDescs[1].texture = CTX.lobbyTextures[1];
        }

        // Cardboard 镜片畸变渲染到屏幕
        CardboardDistortionRenderer_renderEyeToDisplay(
            CTX.distortionRenderer,
            0, 0, 0,
            CTX.screenWidth, CTX.screenHeight,
            &viewsDescs[0], &viewsDescs[1]);

    } catch (const json::exception &e) {
        __android_log_print(ANDROID_LOG_ERROR, "OdyxVR",
            "JSON exception: %s", e.what());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_switchViewerNative(JNIEnv *, jobject) {
    // QR 码扫描 — 需要时手动触发
    CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}
