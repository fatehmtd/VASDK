package com.vasdk.android.tools

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.ImageReader
import android.os.Environment
import android.os.Handler
import android.os.HandlerThread
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CompletableFuture

object CameraTools {

    val toolNames = setOf("capture_photo")

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "capture_photo" -> capturePhoto(context)
        else            -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    private fun hasCamera(context: Context) =
        ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    //  capture_photo 
    // Uses Camera2 ImageReader (no preview surface required).

    private fun capturePhoto(context: Context): String {
        if (!hasCamera(context))
            return JSONObject().put(
                "error",
                "Cannot use the camera: the user has not granted Camera access. " +
                "Tell the user they can enable the Camera permission for this app in Android settings."
            ).toString()

        val result = AsyncBridge.blockingCall<String>(15_000) { future ->
            capturePhotoAsync(context, future)
        }
        return result ?: JSONObject().put("error", "Photo capture timed out or failed").toString()
    }

    private fun capturePhotoAsync(context: Context, future: CompletableFuture<String>) {
        val cameraThread = HandlerThread("vasdk-camera").apply { start() }
        val handler = Handler(cameraThread.looper)

        val cm = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val cameraId = cm.cameraIdList.firstOrNull { id ->
            cm.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: cm.cameraIdList.firstOrNull()

        if (cameraId == null) {
            future.complete(JSONObject().put("error", "No camera found").toString())
            cameraThread.quitSafely()
            return
        }

        val characteristics = cm.getCameraCharacteristics(cameraId)
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(ImageFormat.JPEG)
        val size  = sizes?.minByOrNull { it.width * it.height } ?: run {
            future.complete(JSONObject().put("error", "Cannot determine camera output size").toString())
            cameraThread.quitSafely()
            return
        }

        val reader = ImageReader.newInstance(size.width, size.height, ImageFormat.JPEG, 1)
        val outDir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
            "VASDK"
        ).apply { mkdirs() }
        val stamp  = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val outFile = File(outDir, "vasdk_$stamp.jpg")

        reader.setOnImageAvailableListener({ ir ->
            val image = ir.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val buf   = image.planes[0].buffer
                val bytes = ByteArray(buf.remaining()).also { buf.get(it) }
                outFile.writeBytes(bytes)
                future.complete(JSONObject().apply {
                    put("file_path", outFile.absolutePath)
                    put("width",     size.width)
                    put("height",    size.height)
                    put("size_kb",   outFile.length() / 1024.0)
                }.toString())
            } catch (e: Exception) {
                future.complete(JSONObject().put("error", e.message ?: "write failed").toString())
            } finally {
                image.close()
                reader.close()
                cameraThread.quitSafely()
            }
        }, handler)

        try {
            cm.openCamera(cameraId, object : android.hardware.camera2.CameraDevice.StateCallback() {
                override fun onOpened(device: android.hardware.camera2.CameraDevice) {
                    val session = listOf(reader.surface)
                    device.createCaptureSession(session,
                        object : android.hardware.camera2.CameraCaptureSession.StateCallback() {
                            override fun onConfigured(session: android.hardware.camera2.CameraCaptureSession) {
                                val req = device.createCaptureRequest(
                                    android.hardware.camera2.CameraDevice.TEMPLATE_STILL_CAPTURE
                                ).apply {
                                    addTarget(reader.surface)
                                    set(android.hardware.camera2.CaptureRequest.CONTROL_MODE,
                                        android.hardware.camera2.CaptureRequest.CONTROL_MODE_AUTO)
                                }.build()
                                session.capture(req, null, handler)
                            }
                            override fun onConfigureFailed(session: android.hardware.camera2.CameraCaptureSession) {
                                future.complete(JSONObject().put("error", "Camera session config failed").toString())
                                device.close(); cameraThread.quitSafely()
                            }
                        }, handler)
                }
                override fun onDisconnected(device: android.hardware.camera2.CameraDevice) {
                    device.close(); cameraThread.quitSafely()
                }
                override fun onError(device: android.hardware.camera2.CameraDevice, error: Int) {
                    future.complete(JSONObject().put("error", "Camera error $error").toString())
                    device.close(); cameraThread.quitSafely()
                }
            }, handler)
        } catch (e: Exception) {
            future.complete(JSONObject().put("error", e.message ?: "camera open failed").toString())
            cameraThread.quitSafely()
        }
    }
}
