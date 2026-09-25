package com.orailnoor.droiddesk.x11

import android.app.Activity
import android.content.Context
import android.view.Display
import com.termux.x11.MainActivity
import com.termux.x11.utils.SamsungDexUtils

/**
 * Per-screen desktop scaling. Termux:X11 prefs live only in memory, so the chosen
 * scale is persisted here and pushed into them whenever the screen context changes
 * (fold/unfold, moving the window to DeX or an external display).
 *
 * The X11 resolution is the window size divided by scale / 100, so 100 % is native.
 */
object DisplayProfile {
    enum class Screen(val key: String, val label: String, val defaultScale: Int) {
        /** Samsung DeX or any non-default display: native pixels, sharp text. */
        DESKTOP("desktop", "Monitor", 100),
        /** Tablet-sized panel, e.g. the unfolded inner screen of a Fold. */
        WIDE("wide", "Wide", 200),
        /** Phone-sized panel, e.g. the cover screen of a Fold. */
        NARROW("narrow", "Phone", 150),
    }

    val SCALES = listOf(100, 125, 150, 175, 200)

    private const val PREFS = "droiddesk_display"

    fun detect(activity: Activity): Screen {
        @Suppress("DEPRECATION") // Activity.getDisplay() needs API 30, minSdk is 28.
        val external = activity.windowManager.defaultDisplay.displayId != Display.DEFAULT_DISPLAY
        if (external || SamsungDexUtils.checkDeXEnabled(activity)) return Screen.DESKTOP
        return if (activity.resources.configuration.smallestScreenWidthDp >= 600) Screen.WIDE else Screen.NARROW
    }

    fun scaleFor(context: Context, screen: Screen): Int =
        prefs(context).getInt(screen.key, screen.defaultScale)

    /** Moves the current screen to the next scale step and persists it. */
    fun cycleScale(context: Context, screen: Screen): Int {
        val current = scaleFor(context, screen)
        val next = SCALES.getOrElse(SCALES.indexOf(current) + 1) { SCALES.first() }
        prefs(context).edit().putInt(screen.key, next).apply()
        return next
    }

    /** Must run before LorieView is measured so the X server starts at the chosen resolution. */
    fun apply(context: Context, screen: Screen) {
        val scale = scaleFor(context, screen)
        MainActivity.getPrefs().apply {
            displayResolutionMode.put("scaled")
            displayScale.put(scale)
            displayStretch.put(true)
            scaleTouchpad.put(true)
        }
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
