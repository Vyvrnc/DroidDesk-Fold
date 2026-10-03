package com.orailnoor.droiddesk.x11

import android.view.MotionEvent
import android.view.View
import com.termux.x11.LorieView
import com.termux.x11.MainActivity
import com.termux.x11.input.InputEventSender
import com.termux.x11.input.TouchInputHandler

/** Connects LorieView to the gesture/input implementation imported from Termux:X11. */
class X11InputController(private val lorieView: LorieView) {
    private val inputHandler = TouchInputHandler(
        MainActivity.getInstance(),
        InputEventSender(lorieView),
    )

    var mode: Int = TouchInputHandler.InputMode.TRACKPAD
        private set

    init {
        setMode(mode)
        MainActivity.getInstance().setKeyHandler(inputHandler::sendKeyEvent)
        lorieView.setCallback { width, height, transform ->
            inputHandler.handleInputTransformChanged(width, height, transform)
        }
        lorieView.setOnTouchListener(::handleMotionEvent)
        lorieView.setOnGenericMotionListener(::handleMotionEvent)
        // Captured pointer (see setPointerCapture): relative mouse events, as in Termux:X11.
        lorieView.setOnCapturedPointerListener { view, event -> inputHandler.handleTouchEvent(lorieView, view, event) }
    }

    /**
     * Pointer capture: Android no longer sees the mouse, so DeX does not show its bars when
     * the cursor reaches the top edge; the X server's cursor is the only one. Requested
     * again on the next click when Android dropped it (focus change).
     */
    fun setPointerCapture(enabled: Boolean) {
        val prefs = MainActivity.getPrefs()
        prefs.pointerCapture.put(enabled)
        inputHandler.reloadPreferences(prefs)
        if (enabled) lorieView.requestPointerCapture() else lorieView.releasePointerCapture()
    }

    fun nextMode(): Int {
        val next = when (mode) {
            TouchInputHandler.InputMode.TRACKPAD -> TouchInputHandler.InputMode.SIMULATED_TOUCH
            TouchInputHandler.InputMode.SIMULATED_TOUCH -> TouchInputHandler.InputMode.TOUCH
            else -> TouchInputHandler.InputMode.TRACKPAD
        }
        setMode(next)
        return next
    }

    /** What a finger on the screen does; a physical mouse works the same in every mode. */
    fun modeLabel(): String = when (mode) {
        TouchInputHandler.InputMode.SIMULATED_TOUCH -> "Dotyk: obrazovka"
        TouchInputHandler.InputMode.TOUCH -> "Dotyk: přímý"
        else -> "Dotyk: touchpad"
    }

    fun dispose() {
        inputHandler.dispose()
        MainActivity.getInstance().setKeyHandler(null)
        lorieView.setOnTouchListener(null)
        lorieView.setOnGenericMotionListener(null)
        lorieView.setOnCapturedPointerListener(null)
        lorieView.releasePointerCapture()
        lorieView.setCallback(null)
    }

    private fun setMode(newMode: Int) {
        mode = newMode
        val prefs = MainActivity.getPrefs()
        prefs.touchMode.put(newMode.toString())
        inputHandler.reloadPreferences(prefs)
    }

    private fun handleMotionEvent(view: View, event: MotionEvent): Boolean =
        inputHandler.handleTouchEvent(lorieView, view, event)

    /** Picks up display pref changes (touchpad scaling depends on the resolution mode). */
    fun reloadPreferences() {
        inputHandler.reloadPreferences(MainActivity.getPrefs())
    }
}
