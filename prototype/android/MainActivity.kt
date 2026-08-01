package com.example.alvrvio

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.media.Image
import android.media.ImageReader
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import android.view.TextureView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import java.nio.ByteBuffer

class MainActivity : AppCompatActivity(), SensorEventListener {
    companion object {
        init { System.loadLibrary("alvr_vio") }
    }

    external fun nativeInitSender(host: String, port: Int)
    external fun nativeStartTracking()
    external fun nativeStopTracking()
    external fun nativePushImu(ax: Float, ay: Float, az: Float, gx: Float, gy: Float, gz: Float, timestamp: Long)
    external fun nativePushCameraFrame(data: ByteArray, timestamp: Long, width: Int, height: Int)

    private lateinit var sensorManager: SensorManager
    private var accelSensor: Sensor? = null
    private var gyroSensor: Sensor? = null

    private var cameraDevice: CameraDevice? = null
    private lateinit var reader: ImageReader
    private lateinit var cameraHandler: Handler

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Minimal UIless activity (assumes permissions already granted)
        sensorManager = getSystemService(SENSOR_SERVICE) as SensorManager
        accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

        val handlerThread = HandlerThread("CameraThread")
        handlerThread.start()
        cameraHandler = Handler(handlerThread.looper)

        // Init UDP sender (example: localhost:9944) - change as needed
        nativeInitSender("127.0.0.1", 9944)

        // Start tracking
        nativeStartTracking()

        // Register IMU
        sensorManager.registerListener(this, accelSensor, SensorManager.SENSOR_DELAY_FASTEST)
        sensorManager.registerListener(this, gyroSensor, SensorManager.SENSOR_DELAY_FASTEST)

        // Start camera at low resolution and push frames
        reader = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 2)
        reader.setOnImageAvailableListener({ reader ->
            val image: Image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
            val plane = image.planes[0]
            val buffer: ByteBuffer = plane.buffer
            val bytes = ByteArray(buffer.remaining())
            buffer.get(bytes)
            val timestamp = System.nanoTime()
            nativePushCameraFrame(bytes, timestamp, image.width, image.height)
            image.close()
        }, cameraHandler)

        // Camera open omitted for brevity: integrate with Camera2 to route Surface from reader
        // This file focuses on showing JNI + sensors integration points.
    }

    override fun onSensorChanged(event: android.hardware.SensorEvent?) {
        event ?: return
        val ts = System.nanoTime()
        when(event.sensor.type){
            Sensor.TYPE_ACCELEROMETER -> {
                // store accel; we will combine in push
                // This example simply forwards accel or gyro on arrival which is not synchronized
                nativePushImu(event.values[0], event.values[1], event.values[2], 0f, 0f, 0f, ts)
            }
            Sensor.TYPE_GYROSCOPE -> {
                nativePushImu(0f,0f,0f, event.values[0], event.values[1], event.values[2], ts)
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun onDestroy() {
        super.onDestroy()
        nativeStopTracking()
        sensorManager.unregisterListener(this)
    }
}
