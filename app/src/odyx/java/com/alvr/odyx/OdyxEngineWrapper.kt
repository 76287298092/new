package com.alvr.odyx

import ai.deepmost.odyx.jni.NativeBridge
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.camera2.*
import android.media.Image
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.util.concurrent.atomic.AtomicBoolean

/**
 * ODYX 6DoF 追踪引擎封装。
 *
 * 设计原则：
 * - Kotlin 侧管理所有相机/IMU/引擎生命周期
 * - Rust 侧通过 getPose() 静态方法读取最新位姿
 * - 不重复声明 JNI native 函数，直接使用 ODYX 的 NativeBridge
 */
class OdyxEngineWrapper private constructor() {

    companion object {
        private const val TAG = "OdyxVR"
        private const val CAM_W = 640
        private const val CAM_H = 480
        private const val POSE_POLL_HZ = 60
        private const val POSE_BUF_SIZE = 26

        @Volatile
        private var instance: OdyxEngineWrapper? = null

        /** 应用启动时调用一次 */
        @JvmStatic
        fun init(context: Context) {
            if (instance == null) {
                synchronized(this) {
                    if (instance == null) {
                        instance = OdyxEngineWrapper()
                        instance!!.internalInit(context)
                    }
                }
            }
        }

        /** Rust JNI 调用：获取最新位姿 [x,y,z, qx,qy,qz,qw] */
        @JvmStatic
        fun getPose(): FloatArray {
            return instance?.latestPose ?: floatArrayOf(0f, 0f, 0f, 0f, 0f, 0f, 1f)
        }

        @JvmStatic
        fun shutdown() {
            instance?.internalShutdown()
            instance = null
        }
    }

    // ---- 引擎状态 -------------------------------------------------------
    private var handle: Long = 0
    private val started = AtomicBoolean(false)
    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var sensorManager: SensorManager? = null
    private var sensorListenerRegistered = false

    // 位姿缓冲区 (避免频繁分配)
    private val poseBuf = DoubleArray(POSE_BUF_SIZE)

    // Rust 端读取的最新位姿
    @Volatile
    private var latestPose = floatArrayOf(0f, 0f, 0f, 0f, 0f, 0f, 1f)

    // 标定参数 (可配置)
    private var fx = 300.0
    private var fy = 300.0
    private var cx = 320.0
    private var cy = 240.0

    // ---- 初始化 ---------------------------------------------------------

    private fun internalInit(context: Context) {
        Log.i(TAG, "OdyxEngineWrapper initializing...")

        // 1. 后台线程
        bgThread = HandlerThread("OdyxVR-BG").also { it.start() }
        bgHandler = Handler(bgThread!!.looper)

        // 2. 创建 Native Estimator
        handle = NativeBridge.nativeCreate()
        if (handle == 0L) {
            Log.e(TAG, "nativeCreate returned 0!")
            return
        }
        Log.i(TAG, "Estimator created: handle=$handle")

        // 3. 启动引擎
        NativeBridge.nativeStart(handle)
        started.set(true)

        // 4. 启动传感器
        startSensors(context)

        // 5. 启动相机
        startCamera(context)

        // 6. 启动位姿轮询线程
        startPosePoller()

        Log.i(TAG, "OdyxEngineWrapper initialized")
    }

    // ---- 相机采集 -------------------------------------------------------

    private fun startCamera(context: Context) {
        bgHandler?.post {
            try {
                val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
                var cameraId: String? = null

                // 找后置摄像头
                for (id in manager.cameraIdList) {
                    val chars = manager.getCameraCharacteristics(id)
                    val facing = chars.get(CameraCharacteristics.LENS_FACING)
                    if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                        cameraId = id
                        break
                    }
                }
                // 如果没有后置，用第一个
                if (cameraId == null) cameraId = manager.cameraIdList[0]

                // 解析标定参数
                val chars = manager.getCameraCharacteristics(cameraId)
                val configMap = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    ?: run { Log.e(TAG, "No config map"); return@post }

                // Android 相机内参 (近似值，后续可通过 ODYX 标定获得精确值)
                val arr = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                if (arr != null && arr.size >= 1) {
                    // 假设 35mm 等效焦距，根据 sensor 尺寸调整
                    val sensorSize = chars.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
                    if (sensorSize != null) {
                        fx = arr[0] * CAM_W / sensorSize.width
                        fy = arr[0] * CAM_H / sensorSize.height
                    }
                }
                cx = CAM_W / 2.0
                cy = CAM_H / 2.0

                // ImageReader
                imageReader = ImageReader.newInstance(CAM_W, CAM_H,
                    android.graphics.ImageFormat.YUV_420_888, 2)
                imageReader!!.setOnImageAvailableListener(imageListener, bgHandler)

                // 打开相机
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    manager.openCamera(cameraId, cameraStateCallback, bgHandler)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Camera init error: ${e.message}")
            }
        }
    }

    private val cameraStateCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            cameraDevice = camera
            createCaptureSession()
        }
        override fun onDisconnected(camera: CameraDevice) { camera.close(); cameraDevice = null }
        override fun onError(camera: CameraDevice, error: Int) { camera.close(); cameraDevice = null }
    }

    private fun createCaptureSession() {
        try {
            val surface = imageReader!!.surface
            val builder = cameraDevice!!.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
            builder.addTarget(surface)
            cameraDevice!!.createCaptureSession(
                listOf(surface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        captureSession = session
                        session.setRepeatingRequest(builder.build(), null, bgHandler)
                    }
                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        Log.e(TAG, "Camera config failed")
                    }
                },
                bgHandler
            )
        } catch (e: Exception) {
            Log.e(TAG, "Camera session error: ${e.message}")
        }
    }

    private val imageListener = ImageReader.OnImageAvailableListener { reader ->
        if (!started.get()) return@OnImageAvailableListener
        val image = reader.acquireLatestImage() ?: return@OnImageAvailableListener
        try {
            val yPlane = image.planes[0]
            val buf = yPlane.buffer
            val stride = yPlane.rowStride
            val gray = ByteArray(CAM_W * CAM_H)

            if (stride == CAM_W) {
                buf.get(gray)
            } else {
                val row = ByteArray(stride)
                for (r in 0 until CAM_H) {
                    buf.get(row, 0, stride)
                    System.arraycopy(row, 0, gray, r * CAM_W, CAM_W)
                }
            }

            NativeBridge.nativePushFrame(
                handle, gray, CAM_W, CAM_H, CAM_W,
                image.timestamp * 1000L, 0L, 0.0,
                fx, fy, cx, cy
            )
        } finally {
            image.close()
        }
    }

    // ---- IMU ------------------------------------------------------------

    private fun startSensors(context: Context) {
        sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
        val accel = sensorManager!!.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro = sensorManager!!.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

        if (accel != null) sensorManager!!.registerListener(sensorListener, accel, SensorManager.SENSOR_DELAY_GAME)
        if (gyro != null) sensorManager!!.registerListener(sensorListener, gyro, SensorManager.SENSOR_DELAY_GAME)
        sensorListenerRegistered = true
    }

    private val sensorListener = SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            if (!started.get()) return
            val tNs = event.timestamp * 1000L
            when (event.sensor.type) {
                Sensor.TYPE_ACCELEROMETER -> NativeBridge.nativePushImu(
                    handle, tNs,
                    event.values[0], event.values[1], event.values[2],
                    0f, 0f, 0f
                )
                Sensor.TYPE_GYROSCOPE -> NativeBridge.nativePushImu(
                    handle, tNs,
                    0f, 0f, 0f,
                    event.values[0], event.values[1], event.values[2]
                )
            }
        }
        override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}
    }

    // ---- 位姿轮询 -------------------------------------------------------

    private var pollerThread: Thread? = null

    private fun startPosePoller() {
        pollerThread = Thread({
            while (started.get() && handle != 0L) {
                try {
                    NativeBridge.nativeGetPose(handle, poseBuf)
                    if (poseBuf[0] != 0.0) {
                        // [valid, tNs, qw,qx,qy,qz, px,py,pz]
                        latestPose = floatArrayOf(
                            poseBuf[6].toFloat(),   // px
                            poseBuf[7].toFloat(),   // py
                            poseBuf[8].toFloat(),   // pz
                            poseBuf[3].toFloat(),   // qx
                            poseBuf[4].toFloat(),   // qy
                            poseBuf[5].toFloat(),   // qz
                            poseBuf[2].toFloat()    // qw
                        )
                    }
                } catch (_: Exception) {}
                try { Thread.sleep(1000 / POSE_POLL_HZ) } catch (_: InterruptedException) { break }
            }
        }, "OdyxVR-PosePoller").also { it.start() }
    }

    // ---- 销毁 -----------------------------------------------------------

    private fun internalShutdown() {
        started.set(false)
        pollerThread?.interrupt()

        if (sensorListenerRegistered) {
            sensorManager?.unregisterListener(sensorListener)
        }
        captureSession?.close(); captureSession = null
        cameraDevice?.close(); cameraDevice = null
        imageReader?.close(); imageReader = null

        if (handle != 0L) {
            NativeBridge.nativeStop(handle)
            NativeBridge.nativeDestroy(handle)
            handle = 0L
        }
        bgThread?.quitSafely(); bgThread = null
        Log.i(TAG, "OdyxEngineWrapper shutdown")
    }
}
