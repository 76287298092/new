## ALVR VIO Prototype (updated)

This directory contains a minimal native VIO (visual-inertial odometry) prototype intended to be integrated into an Android app (ALVR Android client). It's a starting point — a lightweight reference that demonstrates the interfaces, JNI glue, a very small tracker stub and a UDP pose sender. It's not a production VIO implementation.

This branch will attempt to integrate OpenVINS as a more realistic VIO backend. Because OpenVINS has several dependencies and is developed upstream, this prototype uses a third_party fetch approach.

Contents
- native/: minimal CMake + native sources (Tracker stub, JNI bridge, UDP sender)
- native/fetch_openvins.sh: script to clone OpenVINS into native/third_party/openvins
- android/MainActivity.kt: Kotlin example showing Camera2 + SensorManager integration and JNI calls
- README.md: this file

Quickstart (with OpenVINS)
1. Fetch OpenVINS (this clones the upstream repository into prototype/native/third_party/openvins):

   cd prototype/native
   ./fetch_openvins.sh

2. Build (example with cmake for native library; integrate into Android Studio via externalNativeBuild):

   mkdir -p build && cd build
   cmake .. -DUSE_OPENVINS=ON -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
   cmake --build . -- -j

3. Integrate the produced library `libalvr_vio.so` into your Android app and wire the JNI calls in MainActivity.kt.

Notes
- If you don't want to fetch OpenVINS, build with -DUSE_OPENVINS=OFF and the stub tracker will be used (functional for testing integration only).
- OpenVINS upstream requires Eigen, OpenCV, and other packages; cross-compiling them for Android may require additional steps — the fetch script only clones the sources; manual dependency resolution may be necessary.
