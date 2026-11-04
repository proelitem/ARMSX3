package com.armsx2.xr

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.addCallback
import androidx.lifecycle.lifecycleScope
import com.armsx2.EmuState
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The immersive half of the app on a headset: the activity Horizon OS composes OpenXR layers
 * for. It owns nothing of the emulator. Main (the panel) stays alive underneath with the
 * runtime, the library and the settings; this activity brings up the OpenXR session, asks the
 * runtime to boot the launch Main parked for it, forwards Bluetooth controller input to Main's
 * handlers (the Touch controllers reach the game inside the core, through OpenXR actions) and
 * leaves when the game stops.
 *
 * Deliberately NOT a MainActivityRuntime: that class's onDestroy shuts the core down and kills
 * the process, which is right for the one activity that hosts the app and wrong for one that
 * comes and goes per game.
 *
 * What the player sees is entirely the core's doing (a quad layer with the game, and the
 * core's own overlays inside it). This activity's window is never shown while immersive, so
 * it has no UI.
 */
class XrEmulationActivity : ComponentActivity() {

    private var xrStarted = false
    private var sawGameRunning = false
    private var leaving = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // System back: leave the game. The runtime's own menu is reached from the pad.
        onBackPressedDispatcher.addCallback(this) {
            if (MainActivityRuntime.eState.value != EmuState.STOPPED) {
                MainActivityRuntime.stop()
            } else {
                leave()
            }
        }

        xrStarted = XrPresentation.start(this)
        if (!xrStarted) {
            // No runtime, no loader in this build, or the core refused: say so and run the game
            // on the panel, which Main will do with the launch it parked.
            Log.w(TAG, "OpenXR could not be started; the game runs on the 2D panel instead")
            Toast.makeText(this, "OpenXR is not available here; showing the game on the panel.", Toast.LENGTH_LONG).show()
            leaving = true
            finish()
            MainActivityRuntime.resumeXrLaunch()
            return
        }

        if (!MainActivityRuntime.resumeXrLaunch()) {
            // Started without a parked launch (a relaunch from recents). Nothing to run.
            Log.w(TAG, "no launch to resume")
            leave()
            return
        }

        // The game stops from the core's menu, from Main, or by dying: in every case eState goes
        // back to STOPPED, and that is this activity's cue to go.
        lifecycleScope.launch {
            while (!leaving) {
                when (MainActivityRuntime.eState.value) {
                    EmuState.RUNNING, EmuState.PAUSED -> sawGameRunning = true
                    EmuState.STOPPED -> if (sawGameRunning) {
                        leave()
                        return@launch
                    }
                    else -> {}
                }
                delay(250)
            }
        }
    }

    private fun leave() {
        if (leaving) return
        leaving = true
        lifecycleScope.launch {
            // Blocks until the renderer has let go of its swapchain, bounded in the core.
            if (xrStarted) withContext(Dispatchers.IO) { XrPresentation.stop() }
            xrStarted = false
            finish()
        }
    }

    override fun onDestroy() {
        // Destroyed by the system with the game still up (task swipe, OS). Stop the game; the
        // core's swapchain goes with it and the session can then be torn down.
        if (!leaving && MainActivityRuntime.eState.value != EmuState.STOPPED) {
            MainActivityRuntime.stop()
        }
        if (xrStarted) {
            xrStarted = false
            Thread { XrPresentation.stop() }.start()
        }
        super.onDestroy()
    }

    // Controller input: Main's handlers hold the pad routing, hotkeys and stick mapping, and
    // they act on process-wide state, so an event is handed to them as if Main had received it.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val host = MainActivityRuntime.instance
        if (host != null && host !== (this as Any) && host.dispatchKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        val host = MainActivityRuntime.instance
        if (host != null && host !== (this as Any) && host.dispatchGenericMotionEvent(ev)) return true
        return super.dispatchGenericMotionEvent(ev)
    }

    companion object {
        private const val TAG = "XrEmulationActivity"

        /** Horizon OS composes OpenXR layers only for an activity carrying this category. */
        private const val VR_CATEGORY = "com.oculus.intent.category.VR"

        fun start(from: Context) {
            val intent = Intent(from, XrEmulationActivity::class.java)
                .addCategory(VR_CATEGORY)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            from.startActivity(intent)
        }
    }
}
