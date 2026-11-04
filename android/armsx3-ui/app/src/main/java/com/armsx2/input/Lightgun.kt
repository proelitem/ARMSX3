package com.armsx2.input

import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import com.armsx3.NativeApp

/**
 * GunCon 2 lightgun, aimed with the touchscreen.
 *
 * The core already emulates the device (`usb_lightgun::GunCon2Device`, `DEVTYPE_GUNCON2`). This is
 * the Android half: it decides which USB port carries it, converts a touch into an aim, and turns
 * taps into trigger pulls.
 *
 * Why the touchscreen rather than a real gun: a CRT lightgun works by timing the scanline sweep,
 * which no LCD reproduces, so every modern lightgun setup is really "a pointing device pretending
 * to be one". A finger is a perfectly good pointing device, and it is the one every phone has.
 *
 * ★ The device type is RESTART-REQUIRED. Swapping a USB device on a live VM is the emulated
 * equivalent of yanking the plug mid-poll: the game has already probed the port and cached what it
 * found. The setting is written immediately so the next boot picks it up.
 */
object Lightgun {
    private const val KEY_ENABLED = "lightgun.enabled"
    private const val KEY_PORT = "lightgun.port"
    /** Off-screen reload margin, as a fraction of the shorter screen edge. */
    private const val EDGE_RELOAD_FRAC = 0.06f

    /** Mirrors the pref so Compose recomposes. */
    val enabled = mutableStateOf(false)
    /** USB port index: 0 = Port 1, 1 = Port 2. */
    val port = mutableStateOf(0)

    fun load() {
        enabled.value = MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, false)
        port.value = MainActivityRuntime.prefs.getInt(KEY_PORT, 0).coerceIn(0, 1)
    }

    /**
     * Persist the choice and push it to the core's USB config.
     *
     * "guncon2" is the core's own type name (`GunCon2Device::TypeName()`), and "None" detaches.
     * Written even with no VM up — it lands in the settings ini, which is what the next boot reads.
     */
    fun setEnabled(on: Boolean) {
        enabled.value = on
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ENABLED, on).apply()
        push()
    }

    fun setPort(p: Int) {
        val newPort = p.coerceIn(0, 1)
        // Detach the old port first, or enabling Port 2 would leave a gun on Port 1 as well.
        if (newPort != port.value && enabled.value) {
            runCatching { com.armsx3.core.ARMSX3.instance.setPadLightgun(port.value, false) }
        }
        port.value = newPort
        MainActivityRuntime.prefs.edit().putInt(KEY_PORT, newPort).apply()
        push()
    }

    private fun push() {
        // The PS3's lightgun is a GunCon 3: a pad with that USB identity, aimed by the core's
        // mouse of the same index (android_mouse_handler.h). Live: the pads are rebuilt.
        runCatching { com.armsx3.core.ARMSX3.instance.setPadLightgun(port.value, enabled.value) }
    }

    /** Re-assert at boot — the ini is authoritative, but this covers a first run with no ini yet. */
    fun applyAtBoot() {
        if (enabled.value) push()
    }

    /**
     * Called when [UsbDevices] changes a port, so aiming follows the device rather than a stale
     * pref. Without this, picking a drum kit on the gun's port would leave the aim layer live and
     * every touch would be swallowed as a shot at a device that isn't there.
     */
    fun syncFromPort(changedPort: Int, type: String) {
        val isGun = type == "guncon2"
        if (isGun) {
            port.value = changedPort
            enabled.value = true
        } else if (changedPort == port.value) {
            enabled.value = false
        }
        MainActivityRuntime.prefs.edit()
            .putBoolean(KEY_ENABLED, enabled.value)
            .putInt(KEY_PORT, port.value)
            .apply()
    }

    // ---- runtime input -------------------------------------------------------

    private var triggerDown = false

    /**
     * Aim at a window pixel coordinate.
     *
     * Passed straight through: `InputManager::UpdatePointerAbsolutePosition` wants window pixels,
     * and our SurfaceView spans the window, so touch coordinates already are those.
     */
    fun aim(x: Float, y: Float, widthPx: Float, heightPx: Float) {
        if (!enabled.value || widthPx <= 0f || heightPx <= 0f) return
        // The core's mouse takes 0..1 from the top-left; the emulated GunCon 3 reads it.
        runCatching { com.armsx3.core.ARMSX3.instance.mouseAim(port.value, x / widthPx, y / heightPx) }
    }

    /**
     * Pull or release the trigger.
     *
     * A touch within [EDGE_RELOAD_FRAC] of an edge fires OFF-SCREEN instead, which is how these
     * games reload — Time Crisis has no reload button, you point away from the screen. Without it
     * the games are unplayable past the first magazine.
     */
    fun trigger(down: Boolean, x: Float, y: Float, widthPx: Float, heightPx: Float) {
        if (!enabled.value) return
        if (down == triggerDown) return
        triggerDown = down
        // The GunCon 3 has no off-screen shot: the PS3 games reload with a button or the pedal
        // (Time Crisis 4 puts cover on the gun's A/B buttons). The edge margin only keeps a press
        // at the very frame from firing. The trigger is the pad's Cross on this port.
        val margin = minOf(widthPx, heightPx) * EDGE_RELOAD_FRAC
        val offscreen = x <= margin || y <= margin || x >= widthPx - margin || y >= heightPx - margin
        if (offscreen && down) return
        runCatching { com.armsx3.Rpcs3Bridge.setPadButton(port.value, android.view.KeyEvent.KEYCODE_BUTTON_A, 0, down) }
    }

    /** Press/release one of the gun's own buttons (A / B / C / Start / Select / Recalibrate). */
    fun button(bind: Int, down: Boolean) {
        if (!enabled.value) return
        // As the pad buttons the GunCon 3 config reads them from (Emu/Io/guncon3_config.h
        // defaults: A1 = L1, B1 = R1, C1 = Select, C2 = Start).
        val keycode = when (bind) {
            NativeApp.GUNCON_A -> android.view.KeyEvent.KEYCODE_BUTTON_L1
            NativeApp.GUNCON_B -> android.view.KeyEvent.KEYCODE_BUTTON_R1
            NativeApp.GUNCON_C -> android.view.KeyEvent.KEYCODE_BUTTON_SELECT
            NativeApp.GUNCON_START -> android.view.KeyEvent.KEYCODE_BUTTON_START
            NativeApp.GUNCON_SELECT -> android.view.KeyEvent.KEYCODE_BUTTON_SELECT
            NativeApp.GUNCON_DPAD_UP -> android.view.KeyEvent.KEYCODE_DPAD_UP
            NativeApp.GUNCON_DPAD_DOWN -> android.view.KeyEvent.KEYCODE_DPAD_DOWN
            NativeApp.GUNCON_DPAD_LEFT -> android.view.KeyEvent.KEYCODE_DPAD_LEFT
            NativeApp.GUNCON_DPAD_RIGHT -> android.view.KeyEvent.KEYCODE_DPAD_RIGHT
            else -> return // recalibrate: nothing to calibrate on an emulated gun
        }
        runCatching { com.armsx3.Rpcs3Bridge.setPadButton(port.value, keycode, 0, down) }
    }

    /** Buttons offered on screen, as (binding, label). Mirrors NetherSX2's gun overlay. */
    fun overlayButtons(): List<Pair<Int, String>> = listOf(
        NativeApp.GUNCON_A to "A",
        NativeApp.GUNCON_B to "B",
        NativeApp.GUNCON_C to "C",
        NativeApp.GUNCON_START to "Start",
        NativeApp.GUNCON_SELECT to "Sel",
        NativeApp.GUNCON_RECALIBRATE to "Cal",
    )
}
