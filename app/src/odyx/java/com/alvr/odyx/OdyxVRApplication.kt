package com.alvr.odyx

import android.Manifest
import android.app.NativeActivity
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * ALVR VR 主 Activity —— 同时驱动 ODYX 6DoF 跟踪。
 *
 * 启动顺序：
 * 1. Activity.onCreate() → 请求权限 → 初始化 ODYX 引擎（相机/IMU/位姿轮询）
 * 2. NativeActivity 自动加载 libalvr_client_openxr.so → 调用 Rust android_main()
 * 3. Rust 端初始化 JNI 桥接 → 读取 ODYX 位姿 → 替换 OpenXR 追踪数据
 */
class OdyxVRApplication : NativeActivity() {

    companion object {
        private const val TAG = "OdyxVR"
        private const val PERMISSION_REQUEST_CODE = 100
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(TAG, "OdyxVRApplication onCreate")

        // 请求相机权限（ODYX VIO 需要）
        requestCameraPermission()
    }

    private fun requestCameraPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.CAMERA),
                PERMISSION_REQUEST_CODE
            )
        } else {
            initOdyx()
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQUEST_CODE) {
            for (i in permissions.indices) {
                if (grantResults[i] == PackageManager.PERMISSION_GRANTED) {
                    Log.i(TAG, "Permission granted: ${permissions[i]}")
                } else {
                    Log.w(TAG, "Permission denied: ${permissions[i]}")
                }
            }
            initOdyx()
        }
    }

    private fun initOdyx() {
        try {
            // 初始化 ODYX 引擎（启动相机、IMU、位姿轮询线程）
            OdyxEngineWrapper.init(applicationContext)
            Log.i(TAG, "ODYx engine started")
        } catch (e: Exception) {
            Log.e(TAG, "ODYx init failed: ${e.message}")
        }
    }

    override fun onDestroy() {
        Log.i(TAG, "Shutting down ODYX")
        OdyxEngineWrapper.shutdown()
        super.onDestroy()
    }
}
