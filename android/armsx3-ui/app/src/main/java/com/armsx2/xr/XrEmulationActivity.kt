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

        // System back: close the game the way every other close route does (closeGame is the
        // chokepoint that remembers an external launch). The runtime's own menu is on the pad.
        onBackPressedDispatcher.addCallback(this) {
            if (MainActivityRuntime.eState.value != EmuState.STOPPED) {
                MainActivityRuntime.closeGame()
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
            // A game that came from a frontend goes back to it: Main's task is finished as well,
            // once this activity is on its way out, so what remains on screen is the frontend.
            MainActivityRuntime.returnToLauncherAfterExternalGame()
        }
    }

    override fun onDestroy() {
        // Destroyed by the system with the game still up (task swipe, OS). Stop the game; the
        // core's swapchain goes with it and the session can then be torn down.
        if (!leaving && MainActivityRuntime.eState.value != EmuState.STOPPED) {
            MainActivityRuntime.closeGame()
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
        if (externalGunKey(event)) return true
        val host = MainActivityRuntime.instance
        if (host != null && host !== (this as Any) && host.dispatchKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if (externalGunMotion(ev)) return true
        val host = MainActivityRuntime.instance
        if (host != null && host !== (this as Any) && host.dispatchGenericMotionEvent(ev)) return true
        return super.dispatchGenericMotionEvent(ev)
    }

    // ---- physical guns -------------------------------------------------------------------
    //
    // In the headset the Touch controller aims, whatever it is mounted on: a physical gun only
    // contributes its controls. Two kinds exist:
    //  - the WonderFitter, a Bluetooth LE accessory with its own protocol (WonderFitterGun);
    //  - any gun the Quest sees as a Bluetooth/USB gamepad or mouse (Arkade, Gun4IR, Samco,
    //    OpenFIRE...). Its trigger is taken here from the usual layouts (mouse button 1, R2, A,
    //    the right trigger axis) and fired through the core's XR input, so it shoots with the
    //    gun hand; every other button reaches the pad through Main's handlers as it would from
    //    a controller (L1/R1 are the GunCon 3's A1/B1, Start, Select).
    // Each new input device is described in the log once, which tells how a gun enumerates.

    private val seenInputDevices = HashSet<Int>()
    private var externalKeyTrigger = false
    private var externalAxisTrigger = false
    private var externalMouseTrigger = false

    private fun describe(device: android.view.InputDevice) {
        if (seenInputDevices.add(device.id)) {
            Log.i(TAG, "input device ${device.id}: '${device.name}' sources=0x${Integer.toHexString(device.sources)} " +
                "vendor=0x${Integer.toHexString(device.vendorId)} product=0x${Integer.toHexString(device.productId)} " +
                "external=${device.isExternal}")
        }
    }

    private fun pushExternalTrigger() {
        val down = externalKeyTrigger || externalAxisTrigger || externalMouseTrigger
        runCatching { com.armsx3.core.ARMSX3.instance.xrGunTrigger(down) }
    }

    private fun externalGunKey(event: KeyEvent): Boolean {
        if (!XrPresentation.lightgun(this)) return false
        val device = event.device ?: return false
        if (device.isVirtual) return false
        describe(device)
        if (!device.isExternal) return false
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_A -> {}
            else -> return false
        }
        if (event.repeatCount > 0) return true
        externalKeyTrigger = event.action == KeyEvent.ACTION_DOWN
        pushExternalTrigger()
        return true
    }

    private fun externalGunMotion(ev: MotionEvent): Boolean {
        if (!XrPresentation.lightgun(this)) return false
        val device = ev.device ?: return false
        if (device.isVirtual) return false
        describe(device)
        if (!device.isExternal) return false
        if (ev.isFromSource(android.view.InputDevice.SOURCE_MOUSE)) {
            val down = (ev.buttonState and MotionEvent.BUTTON_PRIMARY) != 0
            if (down != externalMouseTrigger) {
                externalMouseTrigger = down
                pushExternalTrigger()
            }
            // The pointer position means nothing here (the window is never shown); the
            // event is consumed so it does not become a stray click on Main.
            return true
        }
        if (ev.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)) {
            val pull = maxOf(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS))
            val down = pull > 0.55f
            if (down != externalAxisTrigger) {
                externalAxisTrigger = down
                pushExternalTrigger()
            }
            // Not consumed: the sticks still go to Main's pad mapping.
        }
        return false
    }

    // ---- the WonderFitter's lifetime -----------------------------------------------------
    //
    // The gun accepts one client: Tremu releases it before starting us, we hold it while this
    // activity is in front and release it in onStop, so Tremu gets it back after the game.

    override fun onResume() {
        super.onResume()
        if (XrPresentation.lightgun(this)) {
            if (WonderFitterGun.permitted(this)) {
                WonderFitterGun.start(this)
            } else if (!askedGunPermissions) {
                askedGunPermissions = true
                requestPermissions(WonderFitterGun.PERMISSIONS, REQUEST_GUN_PERMISSIONS)
            }
        }
    }

    override fun onStop() {
        WonderFitterGun.stop()
        super.onStop()
    }

    private var askedGunPermissions = false

    @Deprecated("Deprecated in Java")
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        @Suppress("DEPRECATION")
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_GUN_PERMISSIONS && WonderFitterGun.permitted(this) && XrPresentation.lightgun(this)) {
            WonderFitterGun.start(this)
        }
    }

    companion object {
        private const val TAG = "XrEmulationActivity"
        private const val REQUEST_GUN_PERMISSIONS = 0x5746

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
