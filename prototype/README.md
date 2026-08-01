# ALVR VIO Prototype

This directory contains a minimal native VIO (visual-inertial odometry) prototype intended to be integrated into an Android app (ALVR Android client). It's a starting point — a lightweight reference that demonstrates the interfaces, JNI glue, a very small tracker stub and a UDP pose sender. It's not a production VIO implementation.

Contents
- native/: minimal CMake + native sources (Tracker stub, JNI bridge, UDP sender)
- android/MainActivity.kt: Kotlin example showing Camera2 + SensorManager integration and JNI calls
- README.md: this file

How to integrate
1. Add the native CMakeLists.txt into your Android app's CMake build (or create an Android Studio native module). The native module builds a library named `alvr_vio`.
2. Wire the JNI methods (declared in MainActivity.kt) to call into native code.
3. From the Activity, push camera frames and IMU samples to native via JNI. The native tracker in this prototype will emit periodic pose packets (UDP) to a configured IP/port.

Notes
- This is a minimal prototype for integration/testing only. Replace the stub tracker with a real VIO (OpenVINS / VINS-Mono / ORB-SLAM3) implementation for production.
- The UDP sender uses an unencrypted socket; for integration with ALVR you should reuse ALVR's existing transport (encryption, handshake) or extend it to carry pose packets.

