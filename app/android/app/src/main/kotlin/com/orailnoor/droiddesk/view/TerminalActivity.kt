package com.orailnoor.droiddesk.view

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import com.orailnoor.droiddesk.runtime.LinuxRuntime
import com.termux.terminal.TerminalSession
import com.termux.terminal.TerminalSessionClient
import com.termux.view.TerminalView
import com.termux.view.TerminalViewClient
import java.io.File

/**
 * A real terminal (PTY) inside the app, without XFCE: Termux's terminal-emulator and
 * terminal-view (Apache-2.0). Profiles: "shell" (Termux bash), "debian" (start-debian),
 * "claude" (claude-debian), "codex" (codex-debian). Sessions outlive the activity
 * (TerminalSessions), so leaving and coming back keeps Claude Code running.
 */
class TerminalActivity : Activity() {
    private lateinit var terminalView: TerminalView
    private lateinit var profile: String
    private var fontSize = 0f
    private var ctrlOn = false
    private var altOn = false
    private val modifierButtons = mutableMapOf<String, Button>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        profile = intent.getStringExtra(EXTRA_PROFILE) ?: "shell"
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        val prefs = getSharedPreferences("terminal", MODE_PRIVATE)
        val density = resources.displayMetrics.scaledDensity
        fontSize = prefs.getFloat("font_size", 14f * density)

        terminalView = TerminalView(this, null).apply {
            setBackgroundColor(Color.BLACK)
            isFocusable = true
            isFocusableInTouchMode = true
            setTerminalViewClient(ViewClient())
            setTextSize(fontSize.toInt())
            setTypeface(loadTypeface())
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            addView(terminalView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(extraKeys(), LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ))
        }
        setContentView(root)
        attach()
    }

    override fun onResume() {
        super.onResume()
        TerminalSessions.client.activity = this
        terminalView.requestFocus()
        terminalView.onScreenUpdated()
    }

    override fun onPause() {
        super.onPause()
        if (TerminalSessions.client.activity === this) TerminalSessions.client.activity = null
        getSharedPreferences("terminal", MODE_PRIVATE).edit().putFloat("font_size", fontSize).apply()
    }

    private fun attach() {
        TerminalSessions.client.activity = this
        val session = TerminalSessions.sessionFor(this, profile)
        terminalView.attachSession(session)
        title = titleFor(profile)
    }

    /** Restarts the profile's process after it ended ("Enter" on the finished screen). */
    private fun restart() {
        TerminalSessions.forget(profile)
        attach()
        terminalView.onScreenUpdated()
    }

    private fun loadTypeface(): Typeface = runCatching {
        Typeface.createFromAsset(assets, "flutter_assets/assets/fonts/JetBrainsMono-Regular.ttf")
    }.getOrDefault(Typeface.MONOSPACE)

    // ── Extra keys for touch: Esc, Tab, Ctrl/Alt (sticky for the next key), arrows … ──

    private fun extraKeys(): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.rgb(28, 28, 32))
        }
        val keys = listOf(
            "Esc" to KeyEvent.KEYCODE_ESCAPE, "Tab" to KeyEvent.KEYCODE_TAB, "Ctrl" to -1, "Alt" to -2,
            "←" to KeyEvent.KEYCODE_DPAD_LEFT, "↑" to KeyEvent.KEYCODE_DPAD_UP,
            "↓" to KeyEvent.KEYCODE_DPAD_DOWN, "→" to KeyEvent.KEYCODE_DPAD_RIGHT,
            "Home" to KeyEvent.KEYCODE_MOVE_HOME, "End" to KeyEvent.KEYCODE_MOVE_END,
            "PgUp" to KeyEvent.KEYCODE_PAGE_UP, "PgDn" to KeyEvent.KEYCODE_PAGE_DOWN,
            "/" to -10, "|" to -11, "~" to -12, "-" to -13, "⌨" to -20,
        )
        for ((label, code) in keys) {
            val button = Button(this).apply {
                text = label
                isAllCaps = false
                setTextColor(Color.WHITE)
                setBackgroundColor(Color.TRANSPARENT)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
                minWidth = 0
                minimumWidth = 0
                setPadding(dp(14), dp(8), dp(14), dp(8))
                isFocusable = false
                setOnClickListener { pressExtraKey(label, code) }
            }
            if (code == -1 || code == -2) modifierButtons[label] = button
            row.addView(button)
        }
        return HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(row)
        }
    }

    private fun pressExtraKey(label: String, code: Int) {
        val session = terminalView.currentSession ?: return
        when {
            code == -1 -> { ctrlOn = !ctrlOn; updateModifiers() }
            code == -2 -> { altOn = !altOn; updateModifiers() }
            code == -20 -> showKeyboard()
            code <= -10 -> {
                val char = when (code) { -10 -> '/'; -11 -> '|'; -12 -> '~'; else -> '-' }
                terminalView.inputCodePoint(char.code, ctrlOn, altOn)
                ctrlOn = false; altOn = false; updateModifiers()
            }
            else -> {
                var mod = 0
                if (ctrlOn) mod = mod or com.termux.terminal.KeyHandler.KEYMOD_CTRL
                if (altOn) mod = mod or com.termux.terminal.KeyHandler.KEYMOD_ALT
                terminalView.handleKeyCode(code, mod)
                ctrlOn = false; altOn = false; updateModifiers()
            }
        }
        if (!session.isRunning && code != -20) restart()
        terminalView.requestFocus()
    }

    private fun updateModifiers() {
        modifierButtons["Ctrl"]?.setTextColor(if (ctrlOn) Color.rgb(120, 200, 255) else Color.WHITE)
        modifierButtons["Alt"]?.setTextColor(if (altOn) Color.rgb(120, 200, 255) else Color.WHITE)
    }

    private fun showKeyboard() {
        terminalView.requestFocus()
        getSystemService(InputMethodManager::class.java)?.showSoftInput(terminalView, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    // ── TerminalView callbacks ──

    private inner class ViewClient : TerminalViewClient {
        override fun onScale(scale: Float): Float {
            if (scale < 0.9f || scale > 1.1f) {
                val density = resources.displayMetrics.scaledDensity
                fontSize = (fontSize * scale).coerceIn(8f * density, 32f * density)
                terminalView.setTextSize(fontSize.toInt())
                return 1.0f
            }
            return scale
        }

        override fun onSingleTapUp(e: MotionEvent) = showKeyboard()
        override fun shouldBackButtonBeMappedToEscape() = false
        override fun shouldEnforceCharBasedInput() = true
        override fun shouldUseCtrlSpaceWorkaround() = false
        override fun isTerminalViewSelected() = true
        override fun copyModeChanged(copyMode: Boolean) {}

        override fun onKeyDown(keyCode: Int, e: KeyEvent, session: TerminalSession): Boolean {
            if (!session.isRunning && keyCode == KeyEvent.KEYCODE_ENTER) {
                restart()
                return true
            }
            return false
        }

        override fun onKeyUp(keyCode: Int, e: KeyEvent) = false
        override fun onLongPress(event: MotionEvent) = false
        override fun readControlKey() = ctrlOn
        override fun readAltKey() = altOn
        override fun readShiftKey() = false
        override fun readFnKey() = false

        override fun onCodePoint(codePoint: Int, ctrlDown: Boolean, session: TerminalSession): Boolean {
            if (ctrlOn || altOn) {
                ctrlOn = false
                altOn = false
                runOnUiThread { updateModifiers() }
            }
            return false
        }

        override fun onEmulatorSet() {}
        override fun logError(tag: String, message: String) { Log.e(tag, message) }
        override fun logWarn(tag: String, message: String) { Log.w(tag, message) }
        override fun logInfo(tag: String, message: String) {}
        override fun logDebug(tag: String, message: String) {}
        override fun logVerbose(tag: String, message: String) {}
        override fun logStackTraceWithMessage(tag: String, message: String, e: Exception) { Log.e(tag, message, e) }
        override fun logStackTrace(tag: String, e: Exception) { Log.e(tag, "error", e) }
    }

    fun onSessionText() = terminalView.onScreenUpdated()
    fun onSessionTitle(title: String?) { if (!title.isNullOrBlank()) this.title = title }

    companion object {
        const val EXTRA_PROFILE = "profile"

        fun titleFor(profile: String) = when (profile) {
            "debian" -> "Debian"
            "claude" -> "Claude Code"
            "codex" -> "Codex"
            else -> "Terminál"
        }
    }
}

/** Running terminal sessions, one per profile; they live as long as the app process. */
object TerminalSessions {
    private const val TAG = "TerminalSessions"
    private val sessions = mutableMapOf<String, TerminalSession>()
    val client = SessionClient()

    @Synchronized
    fun sessionFor(context: Context, profile: String): TerminalSession {
        sessions[profile]?.let { if (it.isRunning) return it }
        val runtime = LinuxRuntime(context.applicationContext)
        val env = runtime.terminalEnvironment()
        val prefix = env["PREFIX"] ?: File(context.filesDir, "usr").absolutePath
        val home = env["HOME"] ?: File(context.filesDir, "home").absolutePath
        val bin = "$prefix/bin"
        val (command, args) = when (profile) {
            "debian" -> "$bin/start-debian" to arrayOf("start-debian")
            "claude" -> "$bin/claude-debian" to arrayOf("claude-debian")
            "codex" -> "$bin/codex-debian" to arrayOf("codex-debian")
            else -> "$bin/bash" to arrayOf("-bash")
        }
        val shell = if (File(command).canExecute()) command else "$bin/bash"
        val argv = if (shell == command) args else arrayOf("-bash")
        val session = TerminalSession(
            shell, home, argv, env.map { "${it.key}=${it.value}" }.toTypedArray(), 5000, client,
        )
        sessions[profile] = session
        Log.i(TAG, "Terminal session $profile: $shell")
        return session
    }

    @Synchronized
    fun forget(profile: String) {
        sessions.remove(profile)?.finishIfRunning()
    }

    class SessionClient : TerminalSessionClient {
        @Volatile var activity: TerminalActivity? = null

        override fun onTextChanged(changedSession: TerminalSession) { activity?.onSessionText() }
        override fun onTitleChanged(changedSession: TerminalSession) { activity?.onSessionTitle(changedSession.title) }
        override fun onSessionFinished(finishedSession: TerminalSession) {
            // The emulator shows the exit status; Enter (or any extra key) starts it again.
            activity?.onSessionText()
        }

        override fun onCopyTextToClipboard(session: TerminalSession, text: String) {
            val context = activity ?: return
            context.getSystemService(ClipboardManager::class.java)
                ?.setPrimaryClip(ClipData.newPlainText("terminal", text))
        }

        override fun onPasteTextFromClipboard(session: TerminalSession) {
            val context = activity ?: return
            val text = context.getSystemService(ClipboardManager::class.java)
                ?.primaryClip?.getItemAt(0)?.coerceToText(context)?.toString() ?: return
            session.emulator?.paste(text)
        }

        override fun onBell(session: TerminalSession) {}
        override fun onColorsChanged(session: TerminalSession) { activity?.onSessionText() }
        override fun onTerminalCursorStateChange(state: Boolean) { activity?.onSessionText() }
        override fun getTerminalCursorStyle(): Int? = null
        override fun logError(tag: String, message: String) { Log.e(tag, message) }
        override fun logWarn(tag: String, message: String) { Log.w(tag, message) }
        override fun logInfo(tag: String, message: String) {}
        override fun logDebug(tag: String, message: String) {}
        override fun logVerbose(tag: String, message: String) {}
        override fun logStackTraceWithMessage(tag: String, message: String, e: Exception) { Log.e(tag, message, e) }
        override fun logStackTrace(tag: String, e: Exception) { Log.e(tag, "error", e) }
    }
}
