package com.orailnoor.droiddesk.view

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.graphics.Color
import android.graphics.BitmapFactory
import android.graphics.drawable.GradientDrawable
import android.view.KeyEvent
import android.view.Window
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.SurfaceHolder
import android.view.ViewGroup
import android.view.View
import android.view.MotionEvent
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.util.Log
import android.widget.Toast
import android.widget.Button
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import android.view.Gravity
import android.content.res.ColorStateList
import android.content.res.Configuration
import com.termux.x11.MainActivity as TermuxMainActivity
import com.termux.x11.LorieView
import com.termux.x11.utils.SamsungDexUtils
import com.orailnoor.droiddesk.runtime.LinuxRuntime
import com.orailnoor.droiddesk.runtime.ChrootRuntime
import com.orailnoor.droiddesk.runtime.ClipboardSync
import com.orailnoor.droiddesk.x11.X11ServiceClient
import com.orailnoor.droiddesk.x11.X11InputController
import com.orailnoor.droiddesk.x11.DisplayProfile

class DesktopActivity : Activity() {
    private var lorieView: LorieView? = null
    private var connectionRequested = false
    private var isSetupDone = false
    private var shouldStartSession = false
    private var sessionMode = "termux"
    private var desktopEnv = "xfce4"
    private lateinit var linuxRuntime: LinuxRuntime
    private lateinit var chrootRuntime: ChrootRuntime
    private var clipboardSync: ClipboardSync? = null
    private lateinit var placeholder: FrameLayout
    private var x11ServiceClient: X11ServiceClient? = null
    private var inputController: X11InputController? = null
    private var screen = DisplayProfile.Screen.WIDE
    private var scaleButton: Button? = null
    private var inputModeButton: Button? = null
    private var controlOverlay: LinearLayout? = null
    private var collapsedControl: Button? = null
    private var surfaceCallback: SurfaceHolder.Callback? = null
    private var loadingOverlay: FrameLayout? = null
    private var loadingStatus: TextView? = null
    private var loadingEstimate: TextView? = null
    private var desktopRevealed = false
    private val loadingMessageHandler = Handler(Looper.getMainLooper())
    private var loadingMessageIndex = 0
    private var loadingStartedAt = 0L
    private var estimatedLoadingSeconds = 30
    private val loadingMessages = listOf(
        "Waking up your portable Linux workspace",
        "Connecting Android to your Linux desktop",
        "Starting the desktop engine and services",
        "Loading your apps, icons, and shortcuts",
        "Polishing your panels, wallpaper, and workspace",
        "Almost ready for your next big idea",
    )
    private val loadingMessageTicker = object : Runnable {
        override fun run() {
            val status = loadingStatus ?: return
            val ticker = this
            status.animate().alpha(0f).setDuration(180).withEndAction {
                loadingMessageIndex = (loadingMessageIndex + 1) % loadingMessages.size
                status.text = loadingMessages[loadingMessageIndex]
                status.animate().alpha(1f).setDuration(260).withEndAction {
                    if (!desktopRevealed) {
                        loadingMessageHandler.postDelayed(ticker, 5_000)
                    }
                }.start()
            }.start()
        }
    }
    private val loadingEstimateTicker = object : Runnable {
        override fun run() {
            val estimate = loadingEstimate ?: return
            val elapsedSeconds = ((android.os.SystemClock.elapsedRealtime() - loadingStartedAt) / 1_000).toInt()
            val remaining = (estimatedLoadingSeconds - elapsedSeconds).coerceAtLeast(0)
            estimate.text = if (remaining > 0) {
                "About $remaining seconds remaining"
            } else {
                "Finishing up…"
            }
            estimate.contentDescription = estimate.text
            if (!desktopRevealed) loadingMessageHandler.postDelayed(this, 1_000)
        }
    }

    // The X11 bind/connect had no timeout: when it did not complete after the
    // phone woke up, the loading overlay stayed on "Almost ready" forever although
    // X and XFCE were running. Retry the connection, then reveal the desktop anyway.
    private var watchdogTicks = 0
    private val connectWatchdog = object : Runnable {
        override fun run() {
            if (desktopRevealed || isFinishing || isDestroyed) return
            watchdogTicks++
            when {
                watchdogTicks >= WATCHDOG_MAX_TICKS -> {
                    Log.w(TAG, "Desktop still hidden after ${watchdogTicks * WATCHDOG_TICK_MS / 1000} s, revealing it")
                    revealDesktop(false)
                    return
                }
                !LorieView.connected() && lorieView != null -> {
                    Log.w(TAG, "No X11 connection yet, reconnecting (attempt $watchdogTicks)")
                    x11ServiceClient?.disconnect()
                    x11ServiceClient = null
                    connectionRequested = true
                    connectToX11Service()
                }
            }
            loadingMessageHandler.postDelayed(this, WATCHDOG_TICK_MS)
        }
    }

    // When the desktop ends (crash, logout, stopped from the home screen) the X server keeps
    // showing the last frame and the app looked frozen. Go back to the home screen, which
    // offers to start it again.
    private val desktopWatchdog = object : Runnable {
        override fun run() {
            if (isFinishing || isDestroyed) return
            if (sessionMode != "chroot" && !linuxRuntime.isRunning()) {
                if (linuxRuntime.desktopEndedUnexpectedly()) {
                    Log.w(TAG, "Desktop session ended unexpectedly")
                    Toast.makeText(this@DesktopActivity, "Plocha skončila, spustíš ji z hlavní obrazovky", Toast.LENGTH_LONG).show()
                }
                finish()
                return
            }
            loadingMessageHandler.postDelayed(this, DESKTOP_WATCHDOG_MS)
        }
    }

    companion object {
        private const val TAG = "DesktopActivity"
        private const val DESKTOP_WATCHDOG_MS = 3_000L
        private const val WATCHDOG_TICK_MS = 20_000L
        // Long enough for a cold session start (waitForDesktopReady allows 45 s).
        private const val WATCHDOG_MAX_TICKS = 5
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        linuxRuntime = LinuxRuntime(this)
        chrootRuntime = ChrootRuntime(this)
        shouldStartSession = intent.getBooleanExtra("startSession", false)
        sessionMode = intent.getStringExtra("mode") ?: if (chrootRuntime.hasRoot()) "chroot" else "termux"
        desktopEnv = intent.getStringExtra("de") ?: "xfce4"
        estimatedLoadingSeconds = if (shouldStartSession) 30 else 10

        requestWindowFeature(Window.FEATURE_NO_TITLE)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        placeholder = FrameLayout(this)
        placeholder.setBackgroundColor(Color.BLACK)
        setContentView(placeholder)
        showLoadingOverlay()
        // Some Android/LineageOS builds throw from PhoneWindow.getInsetsController
        // until a decor view has been created by setContentView().
        enableImmersiveMode()

        loadingMessageHandler.postDelayed(connectWatchdog, WATCHDOG_TICK_MS)
        Log.i(TAG, "DesktopActivity created mode=$sessionMode startSession=$shouldStartSession")
    }

    private var captureButton: android.widget.Button? = null

    private fun setPointerCapture(enable: Boolean) {
        getSharedPreferences("desktop_controls", MODE_PRIVATE).edit().putBoolean("pointer_capture", enable).apply()
        inputController?.setPointerCapture(enable)
        captureButton?.text = if (enable) "Myš: zachycená" else "Myš: volná"
        Toast.makeText(
            this,
            if (enable) "Myš zachycena (lišty DeX se neukážou). Uvolnit: Ctrl+Alt+M" else "Myš uvolněna",
            Toast.LENGTH_LONG,
        ).show()
    }

    /** Ctrl+Alt+M toggles pointer capture: with the mouse captured, nothing else here is clickable. */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_M && event.isCtrlPressed && event.isAltPressed) {
            if (event.action == KeyEvent.ACTION_UP) {
                setPointerCapture(!getSharedPreferences("desktop_controls", MODE_PRIVATE).getBoolean("pointer_capture", false))
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enableImmersiveMode()
        if (hasFocus) startClipboardSync() else clipboardSync?.stop()
        updateDexKeyCapture(hasFocus)
        if (hasFocus && !isSetupDone) {
            isSetupDone = true
            Log.i(TAG, "Window focused — setting up LorieView")
            setupLorieView()
        }
    }

    /** Fold/unfold and moving to DeX or an external display arrive here (see configChanges). */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val detected = DisplayProfile.detect(this)
        if (detected == screen) return
        Log.i(TAG, "Screen changed $screen -> $detected")
        screen = detected
        applyDisplayProfile()
        placeholder.requestApplyInsets()
        updateDexKeyCapture(hasWindowFocus())
    }

    private fun applyDisplayProfile() {
        DisplayProfile.apply(this, screen)
        scaleButton?.text = scaleLabel()
        lorieView?.post {
            lorieView?.reapplyDisplaySettings()
            inputController?.reloadPreferences()
        }
    }

    /** Fold screens have a punch-hole camera; without this it covers part of the XFCE panel. */
    private fun applyCutoutInsets(insets: WindowInsets) {
        val cutout = insets.displayCutout
        if (cutout == null || screen == DisplayProfile.Screen.DESKTOP) {
            lorieView?.setContentInsets(0, 0, 0, 0)
        } else {
            lorieView?.setContentInsets(cutout.safeInsetLeft, cutout.safeInsetTop, cutout.safeInsetRight, cutout.safeInsetBottom)
        }
    }

    private fun scaleLabel() = "${screen.label} ${DisplayProfile.scaleFor(this, screen)} %"

    /** In DeX the Meta key (and shortcuts built on it) belongs to Samsung unless we claim it. */
    private fun updateDexKeyCapture(hasFocus: Boolean) {
        SamsungDexUtils.dexMetaKeyCapture(this, hasFocus && screen == DisplayProfile.Screen.DESKTOP)
    }

    override fun onResume() {
        super.onResume()
        enableImmersiveMode()
    }

    private fun startClipboardSync() {
        if (clipboardSync == null) {
            clipboardSync = if (sessionMode == "chroot") {
                ClipboardSync(this, chrootRuntime::readLinuxClipboard, chrootRuntime::writeLinuxClipboard)
            } else {
                ClipboardSync(this, linuxRuntime::readLinuxClipboard, linuxRuntime::writeLinuxClipboard)
            }
        }
        clipboardSync?.start()
    }

    override fun onPause() {
        clipboardSync?.stop()
        super.onPause()
    }

    @Suppress("DEPRECATION")
    private fun enableImmersiveMode() {
        window.statusBarColor = Color.TRANSPARENT
        window.navigationBarColor = Color.TRANSPARENT
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.decorView.windowInsetsController?.apply {
                hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN,
            )
            window.decorView.systemUiVisibility =
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }
    }

    private fun setupLorieView() {
        Log.i(TAG, "Setting up LorieView")
        screen = DisplayProfile.detect(this)
        DisplayProfile.apply(this, screen)
        TermuxMainActivity.getInstance().initLorieView(this)
        lorieView = TermuxMainActivity.getInstance().lorieView

        // Keep Android overlay controls above the X11 SurfaceView.
        lorieView!!.setZOrderOnTop(false)
        placeholder.setBackgroundColor(Color.BLACK)

        val params = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        )
        // TermuxMainActivity retains its LorieView singleton across activity
        // recreation. Detach it from the previous activity's container before
        // attaching it here, otherwise Android throws "child already has a parent".
        (lorieView!!.parent as? ViewGroup)?.removeView(lorieView)
        placeholder.addView(lorieView, params)
        placeholder.setOnApplyWindowInsetsListener { _, insets ->
            applyCutoutInsets(insets)
            insets
        }
        placeholder.requestApplyInsets()
        loadingOverlay?.bringToFront()
        Log.i(TAG, "LorieView added to placeholder")

        // Start X server only after the Surface is actually created/changed.
        surfaceCallback = object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.i(TAG, "LorieView surfaceCreated")
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.i(TAG, "LorieView surfaceChanged ${width}x${height}")
                synchronized(this@DesktopActivity) {
                    if (!connectionRequested) {
                        connectionRequested = true
                        connectToX11Service()
                    }
                }
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.i(TAG, "LorieView surfaceDestroyed")
            }
        }.also { lorieView!!.holder.addCallback(it) }
    }

    private fun connectToX11Service() {
        if (LorieView.connected()) {
            attachDesktopInput()
            return
        }

        x11ServiceClient = X11ServiceClient(
            context = this,
            onConnected = { connectionFd, logcatFd ->
                try {
                    LorieView.connect(connectionFd.detachFd())
                    logcatFd?.let { LorieView.startLogcat(it.detachFd()) }
                    Log.i(TAG, "LorieView connected to the :x11 service process")

                    attachDesktopInput()
                } catch (error: Throwable) {
                    connectionFd.close()
                    logcatFd?.close()
                    showX11Error("Failed to attach LorieView to the X11 service", error)
                }
            },
            onError = ::showX11Error,
        ).also { it.connect() }
    }

    private fun attachDesktopInput() {
        if (inputController == null) {
            inputController = X11InputController(lorieView!!)
            if (getSharedPreferences("desktop_controls", MODE_PRIVATE).getBoolean("pointer_capture", false)) {
                inputController?.setPointerCapture(true)
            }
        }
        addDesktopControls()
        lorieView?.requestFocus()
        if (shouldStartSession) {
            startDesktopSessionIfRequested()
        } else {
            waitForDesktopAndReveal()
        }
    }

    private fun addDesktopControls() {
        if (controlOverlay != null) return
        val density = resources.displayMetrics.density

        fun controlButton(label: String) = Button(this).apply {
            isAllCaps = false
            minWidth = 0
            minHeight = 0
            textSize = 12f
            setTextColor(Color.WHITE)
            setPadding((12 * density).toInt(), 0, (12 * density).toInt(), 0)
            backgroundTintList = ColorStateList.valueOf(Color.argb(220, 28, 38, 52))
            elevation = 6 * density
            text = label
        }

        val dragHandle = controlButton("⋮").apply {
            contentDescription = "Drag desktop controls"
            setPadding((8 * density).toInt(), 0, (8 * density).toInt(), 0)
        }
        val keyboardButton = controlButton("Keyboard").apply {
            setOnClickListener { showKeyboard() }
        }
        inputModeButton = controlButton(inputController?.modeLabel() ?: "Dotyk: touchpad").apply {
            setOnClickListener {
                inputController?.nextMode()
                text = inputController?.modeLabel() ?: "Dotyk: touchpad"
                Toast.makeText(this@DesktopActivity, "$text (fyzická myš funguje stejně)", Toast.LENGTH_SHORT).show()
            }
        }
        scaleButton = controlButton(scaleLabel()).apply {
            contentDescription = "Change desktop scale for this screen"
            setOnClickListener {
                DisplayProfile.cycleScale(this@DesktopActivity, screen)
                applyDisplayProfile()
            }
        }
        val hideButton = controlButton("−").apply {
            contentDescription = "Hide desktop controls"
            setOnClickListener { setControlsCollapsed(true) }
            setPadding((9 * density).toInt(), 0, (9 * density).toInt(), 0)
        }

        // Extra keys like Termux:X11's: an on-screen keyboard has no arrows, Esc
        // or Tab, which the terminal needs. Shown on demand; the choice is kept.
        val preferences = getSharedPreferences("desktop_controls", MODE_PRIVATE)
        val extraKeys = buildExtraKeysRow(::controlButton, density).apply {
            visibility = if (preferences.getBoolean("extra_keys", false)) View.VISIBLE else View.GONE
        }
        val extraKeysButton = controlButton("Keys").apply {
            contentDescription = "Show or hide arrow, Esc, Tab and Ctrl keys"
            setOnClickListener {
                val show = extraKeys.visibility != View.VISIBLE
                extraKeys.visibility = if (show) View.VISIBLE else View.GONE
                preferences.edit().putBoolean("extra_keys", show).apply()
            }
        }

        // DeX shows its bars when the mouse touches the top edge; capturing the pointer keeps
        // the mouse inside the desktop. Released with Ctrl+Alt+M (or this button).
        val captureButton = controlButton(if (preferences.getBoolean("pointer_capture", false)) "Myš: zachycená" else "Myš: volná").apply {
            contentDescription = "Capture the mouse (hides the DeX bars at the top edge)"
            setOnClickListener {
                val enable = !preferences.getBoolean("pointer_capture", false)
                setPointerCapture(enable)
                text = if (enable) "Myš: zachycená" else "Myš: volná"
            }
        }
        this.captureButton = captureButton

        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(dragHandle, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(keyboardButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(inputModeButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(scaleButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(extraKeysButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(captureButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
            addView(hideButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, (42 * density).toInt(),
            ))
        }

        controlOverlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
            addView(buttonRow)
            addView(extraKeys)
        }

        collapsedControl = controlButton("☰").apply {
            contentDescription = "Show desktop controls"
            // Keep this measured so switching from a dragged full overlay can
            // copy absolute coordinates without placing the restore handle off-screen.
            visibility = View.INVISIBLE
        }

        val overlayParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.END,
        ).apply {
            rightMargin = (8 * density).toInt()
            topMargin = (52 * density).toInt()
        }
        val collapsedParams = FrameLayout.LayoutParams(
            (48 * density).toInt(),
            (42 * density).toInt(),
            Gravity.TOP or Gravity.END,
        ).apply {
            rightMargin = overlayParams.rightMargin
            topMargin = overlayParams.topMargin
        }

        placeholder.addView(controlOverlay, overlayParams)
        placeholder.addView(collapsedControl, collapsedParams)
        if (!desktopRevealed) controlOverlay?.visibility = View.INVISIBLE
        dragHandle.setOnTouchListener(dragListener(controlOverlay!!))
        collapsedControl?.setOnTouchListener(dragListener(collapsedControl!!) {
            setControlsCollapsed(false)
        })
        controlOverlay?.bringToFront()
    }

    private var ctrlLatched = false

    /**
     * Esc, Tab, a latching Ctrl, arrows (repeating while held), Home and End,
     * sent as Android key codes the X server maps itself. A latched Ctrl stays
     * pressed for the next key from this row or for typed text, until tapped again
     * or used once by a key of this row.
     */
    private fun buildExtraKeysRow(controlButton: (String) -> Button, density: Float): LinearLayout {
        val repeatHandler = Handler(Looper.getMainLooper())
        fun send(keyCode: Int, down: Boolean) {
            lorieView?.sendKeyEvent(0, keyCode, down)
        }
        lateinit var ctrlButton: Button
        fun releaseCtrlIfLatched() {
            if (!ctrlLatched) return
            ctrlLatched = false
            send(KeyEvent.KEYCODE_CTRL_LEFT, false)
            ctrlButton.backgroundTintList = ColorStateList.valueOf(Color.argb(220, 28, 38, 52))
        }
        fun key(label: String, keyCode: Int, repeat: Boolean) = controlButton(label).apply {
            setPadding((10 * density).toInt(), 0, (10 * density).toInt(), 0)
            val repeater = object : Runnable {
                override fun run() {
                    send(keyCode, true)
                    send(keyCode, false)
                    repeatHandler.postDelayed(this, 50)
                }
            }
            setOnTouchListener { view, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        view.isPressed = true
                        send(keyCode, true)
                        if (repeat) repeatHandler.postDelayed(repeater, 400)
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        view.isPressed = false
                        repeatHandler.removeCallbacks(repeater)
                        send(keyCode, false)
                        releaseCtrlIfLatched()
                        lorieView?.requestFocus()
                        true
                    }
                    else -> false
                }
            }
        }
        ctrlButton = controlButton("Ctrl").apply {
            setPadding((10 * density).toInt(), 0, (10 * density).toInt(), 0)
            setOnClickListener {
                if (ctrlLatched) {
                    releaseCtrlIfLatched()
                } else {
                    ctrlLatched = true
                    send(KeyEvent.KEYCODE_CTRL_LEFT, true)
                    backgroundTintList = ColorStateList.valueOf(Color.argb(230, 37, 99, 235))
                }
                lorieView?.requestFocus()
            }
        }
        val height = (40 * density).toInt()
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            listOf(
                key("Esc", KeyEvent.KEYCODE_ESCAPE, repeat = false),
                key("Tab", KeyEvent.KEYCODE_TAB, repeat = false),
                ctrlButton,
                key("←", KeyEvent.KEYCODE_DPAD_LEFT, repeat = true),
                key("↑", KeyEvent.KEYCODE_DPAD_UP, repeat = true),
                key("↓", KeyEvent.KEYCODE_DPAD_DOWN, repeat = true),
                key("→", KeyEvent.KEYCODE_DPAD_RIGHT, repeat = true),
                key("Home", KeyEvent.KEYCODE_MOVE_HOME, repeat = false),
                key("End", KeyEvent.KEYCODE_MOVE_END, repeat = false),
            ).forEach { addView(it, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, height)) }
        }
    }

    private fun dragListener(target: View, onTap: (() -> Unit)? = null): View.OnTouchListener {
        var downRawX = 0f
        var downRawY = 0f
        var startX = 0f
        var startY = 0f
        var dragged = false
        val threshold = resources.displayMetrics.density * 6

        return View.OnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = event.rawX
                    downRawY = event.rawY
                    startX = target.x
                    startY = target.y
                    dragged = false
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = event.rawX - downRawX
                    val dy = event.rawY - downRawY
                    if (kotlin.math.abs(dx) > threshold || kotlin.math.abs(dy) > threshold) {
                        dragged = true
                    }
                    target.x = (startX + dx).coerceIn(0f, (placeholder.width - target.width).coerceAtLeast(0).toFloat())
                    target.y = (startY + dy).coerceIn(0f, (placeholder.height - target.height).coerceAtLeast(0).toFloat())
                    true
                }
                MotionEvent.ACTION_UP -> {
                    if (!dragged) onTap?.invoke()
                    true
                }
                else -> false
            }
        }
    }

    private fun setControlsCollapsed(collapsed: Boolean) {
        val from = if (collapsed) controlOverlay else collapsedControl
        val to = if (collapsed) collapsedControl else controlOverlay
        to?.x = from?.x ?: 0f
        to?.y = from?.y ?: 0f
        from?.visibility = View.INVISIBLE
        to?.visibility = View.VISIBLE
        to?.bringToFront()
    }

    private fun showKeyboard() {
        val view = lorieView ?: return
        val inputMethod = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        view.requestFocus()
        inputMethod.restartInput(view)
        view.post {
            inputMethod.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    private fun startDesktopSessionIfRequested() {
        if (!shouldStartSession) return
        shouldStartSession = false
        Thread({
            Log.i(TAG, "Starting Linux desktop session after X server connection")
            try {
                if (sessionMode == "chroot") {
                    chrootRuntime.startSession(desktopEnv)
                } else {
                    linuxRuntime.startSession(desktopEnv, "x11")
                }
                val ready = if (sessionMode == "chroot") {
                    chrootRuntime.waitForDesktopReady(desktopEnv)
                } else {
                    linuxRuntime.waitForDesktopReady(desktopEnv)
                }
                revealDesktop(ready)
            } catch (error: Throwable) {
                Log.e(TAG, "Desktop session failed", error)
                revealDesktop(false)
            }
        }, "LinuxDesktopSession").start()
    }

    private fun waitForDesktopAndReveal() {
        Thread({
            val ready = if (sessionMode == "chroot") {
                chrootRuntime.waitForDesktopReady(desktopEnv)
            } else {
                linuxRuntime.waitForDesktopReady(desktopEnv)
            }
            revealDesktop(ready)
        }, "LinuxDesktopReady").start()
    }

    private fun showLoadingOverlay() {
        val density = resources.displayMetrics.density
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
        }

        runCatching {
            assets.open("flutter_assets/assets/icons/logo.png").use { stream ->
                ImageView(this).apply {
                    setImageBitmap(BitmapFactory.decodeStream(stream))
                    scaleType = ImageView.ScaleType.FIT_CENTER
                    content.addView(
                        this,
                        LinearLayout.LayoutParams((112 * density).toInt(), (112 * density).toInt()).apply {
                            bottomMargin = (26 * density).toInt()
                        },
                    )
                }
            }
        }.onFailure { Log.w(TAG, "Loading logo unavailable", it) }

        content.addView(TextView(this).apply {
            text = "DroidDesk"
            textSize = 28f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        })
        content.addView(ProgressBar(this).apply {
            isIndeterminate = true
            indeterminateTintList = ColorStateList.valueOf(Color.WHITE)
            contentDescription = "Loading Linux desktop"
        }, LinearLayout.LayoutParams(
            (42 * density).toInt(),
            (42 * density).toInt(),
        ).apply {
            gravity = Gravity.CENTER_HORIZONTAL
            topMargin = (20 * density).toInt()
        })
        loadingStatus = TextView(this).apply {
            text = loadingMessages.first()
            textSize = 14f
            setTextColor(Color.rgb(190, 198, 215))
            gravity = Gravity.CENTER
        }.also { status ->
            content.addView(status, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = (14 * density).toInt() })
        }
        loadingEstimate = TextView(this).apply {
            text = "About $estimatedLoadingSeconds seconds remaining"
            textSize = 12f
            setTextColor(Color.rgb(130, 143, 164))
            gravity = Gravity.CENTER
            contentDescription = text
        }.also { estimate ->
            content.addView(estimate, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = (8 * density).toInt() })
        }

        loadingOverlay = FrameLayout(this).apply {
            background = GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(Color.rgb(12, 18, 32), Color.rgb(4, 7, 14)),
            )
            addView(content, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER,
            ))
        }.also { overlay ->
            placeholder.addView(overlay, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
        }
        loadingStartedAt = android.os.SystemClock.elapsedRealtime()
        loadingMessageHandler.postDelayed(loadingMessageTicker, 5_000)
        loadingMessageHandler.postDelayed(loadingEstimateTicker, 1_000)
    }

    private fun revealDesktop(ready: Boolean) {
        runOnUiThread {
            if (desktopRevealed || isFinishing || isDestroyed) return@runOnUiThread
            desktopRevealed = true
            loadingMessageHandler.removeCallbacks(loadingMessageTicker)
            loadingMessageHandler.removeCallbacks(loadingEstimateTicker)
            loadingMessageHandler.removeCallbacks(connectWatchdog)
            if (!ready) Log.w(TAG, "Revealing desktop after readiness timeout")
            loadingMessageHandler.removeCallbacks(desktopWatchdog)
            loadingMessageHandler.postDelayed(desktopWatchdog, DESKTOP_WATCHDOG_MS)
            Log.i(TAG, "Desktop revealed ready=$ready")
            loadingOverlay?.animate()
                ?.alpha(0f)
                ?.setDuration(350)
                ?.withEndAction {
                    loadingOverlay?.visibility = View.GONE
                    controlOverlay?.visibility = View.VISIBLE
                    controlOverlay?.bringToFront()
                    lorieView?.requestFocus()
                }
                ?.start()
        }
    }

    private fun showX11Error(message: String, error: Throwable?) {
        Log.e(TAG, message, error)
        Toast.makeText(this, "X11 Error: $message", Toast.LENGTH_LONG).show()
    }

    override fun onDestroy() {
        clipboardSync?.stop()
        clipboardSync = null
        loadingMessageHandler.removeCallbacks(loadingMessageTicker)
        loadingMessageHandler.removeCallbacks(loadingEstimateTicker)
        loadingMessageHandler.removeCallbacks(connectWatchdog)
        loadingMessageHandler.removeCallbacks(desktopWatchdog)
        surfaceCallback?.let { callback -> lorieView?.holder?.removeCallback(callback) }
        surfaceCallback = null
        inputController?.dispose()
        inputController = null
        x11ServiceClient?.disconnect()
        x11ServiceClient = null
        super.onDestroy()
    }
}
