package com.orailnoor.droiddesk.runtime

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.display.DisplayManager
import android.media.ImageReader
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.Settings
import android.system.Os
import android.system.OsConstants
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Display
import com.orailnoor.droiddesk.view.CameraPermissionActivity
import java.io.BufferedReader
import java.io.OutputStream
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * The phone's camera for Linux programs. Termux Firefox (started through firefox-cam) loads
 * libdroiddesk_v4l2.so, a fake /dev/video0, which asks this bridge for frames. The camera
 * runs only while such a client streams; one client at a time.
 *
 * Protocol v1 on the abstract socket "droiddesk.camera", text lines (full description in
 * tools/v4l2-shim/camera_bridge_protocol.h):
 *   open v=1 width=W height=H fps=F format=I420 [facing=front|back] [rotate=D] [mirror=0|1]
 *       -> "ok v=1 width=… slots=3 slot_stride=… data_offset=4096 size=… camera=… facing=…
 *          sensor=WxH rotation=D" with the shared memory fd (SCM_RIGHTS), then one
 *          "frame <slot> <seq> <timestamp>" line per frame and "state paused <why>" /
 *          "state running"; or "err busy|no-permission|no-camera|bad-request|camera-error …".
 *       "close" or closing the socket stops the camera at once.
 *   facing front|back -> "ok": switches the running stream and remembers the choice.
 *   status            -> "ok idle" or "ok streaming …".
 *
 * Frames: Camera2 ImageReader (YUV_420_888) at the smallest supported size that covers the
 * asked one after rotation, converted by camera_jni.c into tightly packed I420 at exactly the
 * asked size (rotated upright for the current display rotation, centre-cropped, scaled; the
 * front camera is not mirrored, browsers mirror their self-view themselves). The ring has three
 * slots with a seqlock each; no acks, the reader copies the newest slot.
 *
 * Android only lets a visible app use the camera (Android 9+), so the picture pauses while
 * DroidDesk is in the background ("state paused"); the session retries every 2 s and resumes.
 * The CAMERA permission is asked on the first open (CameraPermissionActivity); when the
 * dialog cannot be shown, or the user said no, a notification explains what to do.
 */
object CameraBridge {
    private const val TAG = "CameraBridge"
    private const val SOCKET_NAME = "droiddesk.camera"
    private const val PREFS = "droiddesk_camera"
    private const val SLOTS = 3
    private const val DATA_OFFSET = 4096
    private const val PERMISSION_TIMEOUT_S = 30L
    private const val CHANNEL_ID = "droiddesk_camera"
    private const val NOTIFICATION_ID = 7303
    private const val REOPEN_DELAY_MS = 2000L

    private val nativeAvailable: Boolean = runCatching { System.loadLibrary("droiddesk_camera") }.isSuccess

    // camera_jni.c
    @JvmStatic private external fun nativeCreate(size: Int): Int
    @JvmStatic private external fun nativeMap(fd: Int, size: Int): Long
    @JvmStatic private external fun nativeUnmap(address: Long, size: Int)
    @JvmStatic private external fun nativeInitHeader(address: Long, width: Int, height: Int, slots: Int, slotStride: Int, dataOffset: Int)
    @JvmStatic private external fun nativeWriteFrame(
        address: Long, slot: Int, seq: Long, timestamp: Long, y: ByteBuffer, u: ByteBuffer, v: ByteBuffer,
        yRowStride: Int, uvRowStride: Int, uvPixelStride: Int, width: Int, height: Int, rotation: Int, mirror: Boolean,
    ): Int

    @Volatile private var server: LocalServerSocket? = null
    private val clients = java.util.concurrent.ConcurrentHashMap.newKeySet<LocalSocket>()
    private val streaming = AtomicBoolean(false)
    @Volatile private var active: Session? = null

    fun start(context: Context) {
        if (server != null) return
        synchronized(this) {
            if (server != null) return
            try {
                val socket = LocalServerSocket(SOCKET_NAME)
                server = socket
                thread(name = "camera-bridge", isDaemon = true) { serve(context.applicationContext, socket) }
                Log.i(TAG, "Camera bridge started")
            } catch (error: Exception) {
                Log.e(TAG, "Could not start the camera bridge", error)
            }
        }
    }

    /** Ends the server and every stream (the camera stops). */
    fun stop() {
        synchronized(this) {
            val socket = server ?: return
            server = null
            runCatching { socket.close() }
        }
        clients.forEach { runCatching { it.shutdownInput(); it.close() } }
        active?.close()
    }

    private fun serve(context: Context, socket: LocalServerSocket) {
        while (server === socket) {
            val client = try {
                socket.accept()
            } catch (error: Exception) {
                if (server === socket) Log.w(TAG, "Camera bridge accept failed", error)
                continue
            }
            if (client.peerCredentials.uid != android.os.Process.myUid()) {
                Log.w(TAG, "Rejected camera request from uid ${client.peerCredentials.uid}")
                runCatching { client.close() }
                continue
            }
            clients.add(client)
            thread(name = "camera-bridge-client", isDaemon = true) {
                try {
                    handle(context, client)
                } catch (error: Exception) {
                    Log.w(TAG, "Camera client failed", error)
                } finally {
                    clients.remove(client)
                    runCatching { client.close() }
                }
            }
        }
    }

    private fun handle(context: Context, client: LocalSocket) {
        val reader = client.inputStream.bufferedReader()
        val output = client.outputStream
        val line = reader.readLine()?.trim() ?: return
        val words = line.split(' ').filter { it.isNotEmpty() }
        when (words.firstOrNull()) {
            "open" -> openStream(context, client, reader, output, line)
            "facing" -> {
                val facing = words.getOrNull(1)?.takeIf { it == "front" || it == "back" }
                if (facing == null) {
                    write(output, "err bad-request facing front|back")
                } else {
                    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("facing", facing).apply()
                    active?.switchFacing(facing)
                    write(output, "ok")
                }
            }
            "status" -> write(output, active?.describe() ?: "ok idle")
            else -> write(output, "err bad-request $line")
        }
    }

    private fun write(output: OutputStream, line: String): Boolean = runCatching {
        synchronized(output) {
            output.write("$line\n".toByteArray())
            output.flush()
        }
    }.isSuccess

    class Request(
        val width: Int,
        val height: Int,
        val fps: Int,
        val facing: String?,
        val rotate: Int,
        val mirror: Boolean,
    )

    private fun parse(line: String): Any {
        val fields = line.split(' ').drop(1).mapNotNull {
            val eq = it.indexOf('=')
            if (eq > 0) it.substring(0, eq) to it.substring(eq + 1) else null
        }.toMap()
        if (fields["v"] != "1") return "bad-request protocol version ${fields["v"]} (this app speaks v=1)"
        if (fields["format"] != "I420") return "bad-request only format=I420"
        val width = fields["width"]?.toIntOrNull() ?: 0
        val height = fields["height"]?.toIntOrNull() ?: 0
        val fps = fields["fps"]?.toIntOrNull() ?: 30
        if (width !in 16..4096 || height !in 16..4096 || width % 2 != 0 || height % 2 != 0 || fps !in 1..60) {
            return "bad-request size ${width}x$height at $fps fps"
        }
        val facing = fields["facing"]?.takeIf { it == "front" || it == "back" }
        val rotate = ((fields["rotate"]?.toIntOrNull() ?: 0) / 90 * 90 % 360 + 360) % 360
        return Request(width, height, fps, facing, rotate, fields["mirror"] == "1")
    }

    private fun openStream(context: Context, client: LocalSocket, reader: BufferedReader, output: OutputStream, line: String) {
        val request = parse(line)
        if (request !is Request) {
            write(output, "err $request")
            return
        }
        if (!claim()) {
            write(output, "err busy another Linux program is using the camera")
            return
        }
        var session: Session? = null
        try {
            if (!ensurePermission(context)) {
                write(output, "err no-permission DroidDesk may not use the camera (see the Android notification)")
                return
            }
            val facing = request.facing
                ?: context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("facing", "front") ?: "front"
            session = Session(context, client, output, request, facing)
            active = session
            val error = session.start()
            if (error != null) {
                Log.w(TAG, "Camera stream not started: $error")
                write(output, "err $error")
                return
            }
            // Until "close" or the client goes away.
            while (true) {
                val next = reader.readLine()?.trim() ?: break
                if (next == "close") break
            }
        } finally {
            session?.close()
            if (active === session) active = null
            streaming.set(false)
        }
    }

    /**
     * One stream at a time. A stream that is just ending (STREAMOFF and STREAMON for another
     * size) gets 1.5 s before a new client is turned away as busy.
     */
    private fun claim(): Boolean {
        repeat(30) {
            if (streaming.compareAndSet(false, true)) return true
            Thread.sleep(50)
        }
        return false
    }

    // ── Permission ──

    @Volatile private var permissionShown: CountDownLatch? = null
    @Volatile private var permissionAnswered: CountDownLatch? = null

    /** CameraPermissionActivity is on screen. */
    fun permissionDialogShown() {
        permissionShown?.countDown()
    }

    /** CameraPermissionActivity got its answer (or went away without one). */
    fun permissionDialogAnswered() {
        permissionShown?.countDown()
        permissionAnswered?.countDown()
    }

    private fun granted(context: Context) =
        context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    /**
     * Asks for CAMERA through CameraPermissionActivity (a service cannot show the dialog). When
     * Android does not let the activity start (DroidDesk in the background) or the user says no,
     * a notification tells what to do; the client gets "no-permission" and shows its test pattern.
     */
    private fun ensurePermission(context: Context): Boolean {
        if (granted(context)) return true
        val shown = CountDownLatch(1)
        val answered = CountDownLatch(1)
        permissionShown = shown
        permissionAnswered = answered
        try {
            runCatching {
                context.startActivity(
                    Intent(context, CameraPermissionActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            }.onFailure { Log.w(TAG, "Could not start the permission activity", it) }
            if (!shown.await(3, TimeUnit.SECONDS)) {
                notifyPermission(context, ask = true)
                return false
            }
            answered.await(PERMISSION_TIMEOUT_S, TimeUnit.SECONDS)
        } finally {
            permissionShown = null
            permissionAnswered = null
        }
        val ok = granted(context)
        if (ok) cancelNotification(context) else notifyPermission(context, ask = false)
        return ok
    }

    private fun notifyPermission(context: Context, ask: Boolean) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Kamera pro Linux", NotificationManager.IMPORTANCE_DEFAULT),
        )
        val intent = if (ask) {
            Intent(context, CameraPermissionActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        } else {
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        val pending = PendingIntent.getActivity(
            context, if (ask) 1 else 2, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val text = if (ask) {
            "Linuxová aplikace chce použít kameru. Klepnutím povolíte DroidDesk přístup ke kameře, pak kameru v aplikaci zapněte znovu."
        } else {
            "DroidDesk nemá oprávnění ke kameře. Klepnutím otevřete nastavení aplikace a v části Oprávnění povolte Fotoaparát."
        }
        val notification = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("Kamera pro Linux")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setContentIntent(pending)
            .setAutoCancel(true)
            .build()
        runCatching { manager.notify(NOTIFICATION_ID, notification) }
    }

    private fun cancelNotification(context: Context) {
        runCatching { context.getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION_ID) }
    }

    // ── Stream ──

    private class Session(
        val context: Context,
        val client: LocalSocket,
        val output: OutputStream,
        val request: Request,
        @Volatile var facing: String,
    ) {
        private val handlerThread = HandlerThread("camera-bridge-session").apply { start() }
        private val handler = Handler(handlerThread.looper)
        private val manager = context.getSystemService(CameraManager::class.java)
        private val displays = context.getSystemService(DisplayManager::class.java)

        // Shared memory ring
        private val frameSize = request.width * request.height * 3 / 2
        private val slotStride: Int
        private val size: Int
        private var fd = -1
        private var address = 0L

        // Camera state, touched only on the handler thread
        private var device: CameraDevice? = null
        private var captureSession: CameraCaptureSession? = null
        private var imageReader: ImageReader? = null
        private var opening: ((String?) -> Unit)? = null
        private var cameraId = ""
        private var sensorSize = Size(0, 0)
        private var sensorOrientation = 0
        private var frontLens = true
        private var paused = false
        private var seq = 0L
        private var lastFrameNs = 0L
        private val periodNs = 1_000_000_000L / request.fps

        @Volatile private var closed = false
        @Volatile private var started = false
        @Volatile private var displayRotation = 0

        private val displayListener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {}
            override fun onDisplayRemoved(displayId: Int) {}
            override fun onDisplayChanged(displayId: Int) {
                if (displayId == Display.DEFAULT_DISPLAY) readDisplayRotation()
            }
        }

        init {
            val page = Os.sysconf(OsConstants._SC_PAGESIZE).toInt().coerceAtLeast(4096)
            slotStride = (frameSize + page - 1) / page * page
            size = DATA_OFFSET + SLOTS * slotStride
        }

        private fun readDisplayRotation() {
            displayRotation = runCatching { displays.getDisplay(Display.DEFAULT_DISPLAY)?.rotation }.getOrNull() ?: 0
        }

        /** Clockwise rotation that makes the sensor image upright on the current display. */
        private fun rotation(): Int {
            val display = displayRotation * 90
            // Display.rotation turns the screen counter-clockwise, so the back camera subtracts it
            // and the (unmirrored) front camera adds it, as in Camera.setDisplayOrientation without
            // its mirror step. The signs were swapped in fold.77/78: both cameras upside down on
            // the unfolded Fold 7 (sensor 90/270, display rotation 90).
            val auto = if (frontLens) (sensorOrientation + display) % 360 else (sensorOrientation - display + 360) % 360
            return (auto + request.rotate) % 360
        }

        /** Opens the camera and hands the ring to the client. null = streaming, else "code text". */
        fun start(): String? {
            if (!nativeAvailable) return "camera-error libdroiddesk_camera.so is missing from the APK"
            fd = nativeCreate(size)
            if (fd < 0) return "camera-error shared memory (errno ${-fd})"
            address = nativeMap(fd, size)
            if (address == 0L) return "camera-error mmap of the shared memory failed"
            nativeInitHeader(address, request.width, request.height, SLOTS, slotStride, DATA_OFFSET)
            readDisplayRotation()
            displays.registerDisplayListener(displayListener, handler)

            val done = CountDownLatch(1)
            var error: String? = null
            handler.post {
                opening = { result ->
                    error = result
                    done.countDown()
                }
                openCamera()
            }
            if (!done.await(10, TimeUnit.SECONDS)) return "camera-error the camera did not open within 10 s"
            error?.let { return it }

            val line = "ok v=1 width=${request.width} height=${request.height} fps=${request.fps} format=I420 " +
                "slots=$SLOTS slot_stride=$slotStride data_offset=$DATA_OFFSET size=$size camera=$cameraId " +
                "facing=$facing sensor=${sensorSize.width}x${sensorSize.height} rotation=${rotation()}"
            val sent = ParcelFileDescriptor.adoptFd(fd).use { shared ->
                fd = -1
                runCatching {
                    synchronized(output) {
                        client.setFileDescriptorsForSend(arrayOf(shared.fileDescriptor))
                        output.write("$line\n".toByteArray())
                        output.flush()
                        client.setFileDescriptorsForSend(null)
                    }
                }.isSuccess
            }
            if (!sent) return "camera-error the client went away"
            Log.i(TAG, "Streaming: $line")
            started = true
            return null
        }

        fun describe(): String =
            "ok streaming width=${request.width} height=${request.height} fps=${request.fps} camera=$cameraId " +
                "facing=$facing sensor=${sensorSize.width}x${sensorSize.height} rotation=${rotation()}" +
                if (paused) " paused" else ""

        fun switchFacing(next: String) {
            handler.post {
                if (closed || next == facing) return@post
                facing = next
                closeCamera()
                opening = { result -> if (result == null) resumed() else lost(result) }
                openCamera()
            }
        }

        private fun send(line: String) {
            if (!write(output, line)) {
                // The client is gone; the client thread sees the end of its input and closes us.
                runCatching { client.shutdownInput() }
            }
        }

        /** Calls the pending open callback once; later failures count as a lost camera. */
        private fun finishOpen(result: String?) {
            val callback = opening
            opening = null
            when {
                callback != null -> callback(result)
                result != null -> lost(result)
            }
        }

        private fun resumed() {
            if (paused) {
                paused = false
                send("state running")
                Log.i(TAG, "Camera running again")
            }
        }

        /** The camera stopped (taken by another app, DroidDesk in the background, error): retry. */
        private fun lost(reason: String) {
            if (closed) return
            closeCamera()
            if (!paused) {
                paused = true
                send("state paused ${reason.substringAfter(' ')}")
                Log.i(TAG, "Camera paused: $reason")
            }
            handler.postDelayed({
                if (!closed && device == null && opening == null) {
                    opening = { result -> if (result == null) resumed() else lost(result) }
                    openCamera()
                }
            }, REOPEN_DELAY_MS)
        }

        private fun pickCamera(): String? {
            val wanted = if (facing == "back") CameraCharacteristics.LENS_FACING_BACK else CameraCharacteristics.LENS_FACING_FRONT
            val usable = manager.cameraIdList.filter { id ->
                val map = manager.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                map?.getOutputSizes(ImageFormat.YUV_420_888)?.isNotEmpty() == true
            }
            return usable.firstOrNull { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == wanted }
                ?: usable.firstOrNull()
        }

        /**
         * The smallest YUV size that runs at the asked fps and, rotated and centre-cropped to the
         * asked aspect ratio, still covers the asked size (no upscaling); else the largest one.
         */
        private fun pickSize(characteristics: CameraCharacteristics, rotation: Int): Size {
            val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val all = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
            if (all.isEmpty()) return Size(request.width, request.height)
            val fast = all.filter {
                val minFrame = map!!.getOutputMinFrameDuration(ImageFormat.YUV_420_888, it)
                minFrame <= 0L || minFrame <= periodNs * 105 / 100
            }.ifEmpty { all }
            val w = request.width.toLong()
            val h = request.height.toLong()
            fun covers(s: Size): Boolean {
                val rw = (if (rotation % 180 != 0) s.height else s.width).toLong()
                val rh = (if (rotation % 180 != 0) s.width else s.height).toLong()
                val cropW = if (rw * h > rh * w) rh * w / h else rw
                val cropH = if (rw * h > rh * w) rh else rw * h / w
                return cropW >= w && cropH >= h
            }
            return fast.filter(::covers).minByOrNull { it.width.toLong() * it.height }
                ?: fast.maxByOrNull { it.width.toLong() * it.height }!!
        }

        /** An AE range whose top is the asked fps, letting AE go down to half of it in the dark. */
        private fun pickFpsRange(characteristics: CameraCharacteristics): Range<Int>? {
            val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.toList().orEmpty()
            val fps = request.fps
            val pool = ranges.filter { it.upper == fps }.ifEmpty { ranges.filter { it.upper > fps } }.ifEmpty { ranges }
            return pool.filter { it.lower * 2 >= fps }.minByOrNull { it.lower } ?: pool.maxByOrNull { it.lower }
        }

        @SuppressLint("MissingPermission")
        private fun openCamera() {
            if (closed) return finishOpen("camera-error stream closed")
            val id = try {
                pickCamera()
            } catch (error: Exception) {
                return finishOpen("camera-error ${error.message}")
            } ?: return finishOpen("no-camera Android lists no usable camera")
            try {
                openCamera(id)
            } catch (error: Exception) {
                // Camera removed or the camera service died between listing and opening.
                imageReader?.close()
                imageReader = null
                finishOpen("camera-error ${error.message}")
            }
        }

        @SuppressLint("MissingPermission")
        private fun openCamera(id: String) {
            val characteristics = manager.getCameraCharacteristics(id)
            cameraId = id
            sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
            frontLens = characteristics.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
            val chosen = pickSize(characteristics, rotation())
            sensorSize = chosen
            val range = pickFpsRange(characteristics)
            val continuousAf = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
                ?.contains(CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO) == true
            val reader = ImageReader.newInstance(chosen.width, chosen.height, ImageFormat.YUV_420_888, 3)
            reader.setOnImageAvailableListener({ onImage(it) }, handler)
            imageReader = reader
            Log.i(TAG, "Opening camera $id ($facing) at ${chosen.width}x${chosen.height}, fps range $range, rotation ${rotation()}")
            try {
                manager.openCamera(id, object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        if (closed || imageReader !== reader) {
                            camera.close()
                            return
                        }
                        device = camera
                        createSession(camera, reader, range, continuousAf)
                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        camera.close()
                        if (imageReader !== reader) return // an older attempt (facing switch)
                        if (device === camera || device == null) finishOpen("camera-error the camera was taken by another app or DroidDesk went to the background")
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        camera.close()
                        val why = when (error) {
                            CameraDevice.StateCallback.ERROR_CAMERA_IN_USE -> "the camera is in use by another app"
                            CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE -> "too many cameras open"
                            CameraDevice.StateCallback.ERROR_CAMERA_DISABLED -> "the camera is disabled (policy or privacy switch)"
                            CameraDevice.StateCallback.ERROR_CAMERA_DEVICE -> "camera device error"
                            else -> "camera service error $error"
                        }
                        if (imageReader !== reader) return // an older attempt (facing switch)
                        if (device === camera || device == null) finishOpen("camera-error $why")
                    }
                }, handler)
            } catch (error: CameraAccessException) {
                finishOpen("camera-error ${error.message}")
            } catch (error: SecurityException) {
                finishOpen("no-permission ${error.message}")
            } catch (error: IllegalArgumentException) {
                finishOpen("no-camera ${error.message}")
            }
        }

        @Suppress("DEPRECATION")
        private fun createSession(camera: CameraDevice, reader: ImageReader, range: Range<Int>?, continuousAf: Boolean) {
            try {
                camera.createCaptureSession(listOf(reader.surface), object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        if (closed || device !== camera) {
                            session.close()
                            return
                        }
                        captureSession = session
                        try {
                            val builder = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
                            builder.addTarget(reader.surface)
                            range?.let { builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                            if (continuousAf) {
                                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                            }
                            session.setRepeatingRequest(builder.build(), null, handler)
                            finishOpen(null)
                        } catch (error: Exception) {
                            finishOpen("camera-error ${error.message}")
                        }
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        finishOpen("camera-error the camera refused ${reader.width}x${reader.height}")
                    }
                }, handler)
            } catch (error: Exception) {
                finishOpen("camera-error ${error.message}")
            }
        }

        private fun onImage(reader: ImageReader) {
            val image = try {
                reader.acquireLatestImage()
            } catch (error: IllegalStateException) {
                null
            } ?: return
            try {
                if (!started || closed || address == 0L) return
                val now = SystemClock.elapsedRealtimeNanos()
                // The camera may run faster than asked (AE range); keep to the asked fps.
                if (lastFrameNs != 0L && now - lastFrameNs < periodNs * 3 / 4) return
                lastFrameNs = now
                val planes = image.planes
                val slot = (seq % SLOTS).toInt()
                val next = seq + 1
                val result = nativeWriteFrame(
                    address, slot, next, image.timestamp, planes[0].buffer, planes[1].buffer, planes[2].buffer,
                    planes[0].rowStride, planes[1].rowStride, planes[1].pixelStride, image.width, image.height,
                    rotation(), request.mirror,
                )
                if (result != 0) {
                    Log.w(TAG, "Frame conversion failed: $result")
                    return
                }
                seq = next
                send("frame $slot $next ${image.timestamp}")
            } finally {
                image.close()
            }
        }

        private fun closeCamera() {
            runCatching { captureSession?.close() }
            captureSession = null
            runCatching { device?.close() }
            device = null
            runCatching { imageReader?.close() }
            imageReader = null
            opening = null
        }

        /** Stops the camera and frees the ring; idempotent, waits for the camera thread. */
        @Synchronized
        fun close() {
            if (closed) return
            closed = true
            val done = CountDownLatch(1)
            val posted = handler.post {
                closeCamera()
                runCatching { displays.unregisterDisplayListener(displayListener) }
                if (address != 0L) nativeUnmap(address, size)
                address = 0L
                if (fd >= 0) runCatching { ParcelFileDescriptor.adoptFd(fd).close() }
                fd = -1
                done.countDown()
            }
            if (posted) done.await(3, TimeUnit.SECONDS)
            handlerThread.quitSafely()
            Log.i(TAG, "Camera stream closed after $seq frames")
        }
    }
}
