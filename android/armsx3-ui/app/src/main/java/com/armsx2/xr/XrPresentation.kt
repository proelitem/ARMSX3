package com.armsx2.xr

import android.app.Activity
import android.content.Context
import android.util.Log
import com.armsx2.HeadsetProfile
import net.rpcsx.RPCSX

/**
 * The virtual-screen mode on a headset: the PS3's picture on a quad in front of the player,
 * composed by the OpenXR runtime (android/src/xr in the core), instead of the flat panel
 * Horizon OS gives a 2D app.
 *
 * On by default on a headset, since it is the point of running there; the switch in the
 * renderer settings turns it off for anyone who prefers the panel. Never on elsewhere.
 */
object XrPresentation {
    private const val TAG = "XrPresentation"
    private const val PREFS = "ARMSX2"
    private const val KEY_ENABLED = "xr.virtualScreen"

    /** Screen placement, metres. 2.4 m wide at 2 m is a large television at couch distance. */
    const val SCREEN_DISTANCE_M = 2.0f
    const val SCREEN_WIDTH_M = 2.4f

    /** Quad resolution. The PS3 outputs 720p; the extra pixels keep the renderer's own scaling
     *  on the sharp side of the compositor's resampling, which is bilinear. */
    const val QUAD_WIDTH = 1920
    const val QUAD_HEIGHT = 1080

    fun enabled(context: Context): Boolean =
        HeadsetProfile.isHeadset() &&
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_ENABLED, true)

    fun setEnabled(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_ENABLED, on).apply()
    }

    /** Whether a launch from [host] should go through XrEmulationActivity. */
    fun shouldRoute(host: Context?): Boolean = host != null && enabled(host)

    /** Installs the renderer hooks for [activity]. False means the game runs on the panel. */
    fun start(activity: Activity): Boolean {
        val ok = runCatching {
            RPCSX.instance.xrStart(activity, SCREEN_DISTANCE_M, SCREEN_WIDTH_M, QUAD_WIDTH, QUAD_HEIGHT)
        }.getOrElse {
            // An older JNI library without the binding: UnsatisfiedLinkError.
            Log.w(TAG, "xrStart unavailable", it)
            false
        }
        Log.i(TAG, "xrStart -> $ok")
        return ok
    }

    /** May block for a moment (the core waits for its swapchain to be gone), so not on the
     *  main thread. */
    fun stop() {
        runCatching { RPCSX.instance.xrStop() }.onFailure { Log.w(TAG, "xrStop unavailable", it) }
    }

    fun active(): Boolean = runCatching { RPCSX.instance.xrActive() }.getOrDefault(false)
}
