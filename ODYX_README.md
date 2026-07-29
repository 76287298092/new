# ODYX 6DoF 集成到 PhoneVR

将 ODYX VIO-SLAM 6DoF 追踪集成到 PhoneVR 的 ALVR 串流中，
让普通安卓手机变成带 6DoF 头追的 PC VR 显示器。

## 修改了什么

### 核心改动：`app/src/main/cpp/alvr_main.cpp`

- 移除 `CardboardHeadTracker`（3DoF 陀螺仪追踪）
- 添加 `getOdyxPose()` 通过 JNI 调用 ODYX 6DoF 位姿
- 替换 `updateViewConfigs()` 中的位姿来源
- 保留 Cardboard 镜片畸变渲染（VR 盒子透镜补偿）
- 保留 ALVR C API 网络层（串流不变）

### 新增文件

```
app/src/odyx/java/com/alvr/odyx/
├── OdyxEngineWrapper.kt    # ODYX 引擎管理 (Camera2 + IMU + 位姿轮询)
└── OdyxVRApplication.kt    # 主 Activity (权限请求 + 引擎启动)

app/src/odyx/libs/arm64-v8a/
└── libodyx.so               # ODYX 预编译 native 库 (需自行编译)
```

### build.gradle 改动

添加了 `sourceSets`，指向 ODYX 的 Java 源码和 jniLibs。

## 编译步骤

### 1. 编译 ODYX Native 库

```bash
cd E:\新建文件夹\ODYX-VISUAL-SLAM

# 安装依赖 (需 Python3 + CMake)
pip install -r scripts/requirements.txt
bash scripts/fetch_deps.sh
bash scripts/build_boost_android.sh

# 用 Android Studio 打开 ODYX 项目
# 在 gradle.properties 中设置:
#   odyx.opencv.sdk=/path/to/OpenCV-android-sdk/sdk/native/jni
# 然后编译：
./gradlew assembleDebug

# 复制产物
cp app/build/intermediates/cxx/*/arm64-v8a/libodyx.so \
   ../PhoneVR/app/src/odyx/libs/arm64-v8a/
```

### 2. 编译 PhoneVR

```bash
cd E:\新建文件夹\PhoneVR\code\mobile\android\PhoneVR
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 配置 ODYX 标定

首次使用前，需要 ODYX 的相机内参标定：
1. 打印一个棋盘格（默认 9x6 格子，25mm 边长）
2. 运行 ODYX 的标定模式，拍摄 10-20 张不同角度的照片
3. 标定结果自动保存到手机的 `calib/` 目录

## 日志查看

```bash
adb logcat -s OdyxVR
adb logcat -s ALVR
```
