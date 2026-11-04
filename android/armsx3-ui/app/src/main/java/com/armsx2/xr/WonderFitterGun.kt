package com.armsx2.xr

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import android.view.KeyEvent
import com.armsx3.Rpcs3Bridge
import com.armsx3.core.ARMSX3
import java.util.UUID

/**
 * The WonderFitter gun (M17 pistol, M416 rifle): a Bluetooth LE accessory that holds a Touch
 * controller. The controller aims (the core's lightgun ray, right hand); the gun itself only
 * tells us what its mechanics did, as notifications on characteristic FFF1 of service FFF0:
 *
 *     shot      5B SS 07 07 CK 5D   SS = sequence number, the frame is sent three times per shot
 *     magazine  5C MM CK 5D         MM: 00 removed, 01 inserted
 *     bolt      60 MM CK 5D         M416 charging handle: 01 pulled, 00 released
 *
 * CK is the low byte of the sum of the bytes before it. There is no "trigger released" frame:
 * in full auto a shot frame arrives about every 100 ms, so the trigger counts as held while
 * the last shot is recent. This is the protocol Tremu (the launcher) speaks to the same gun.
 *
 * The gun accepts one client. Tremu lets go of it before starting ARMSX3 and takes it back when
 * it is in front again, so this side connects while the headset activity is in front and
 * releases in onStop. Without a gun nothing here changes anything.
 *
 * What the mechanics do in the game, on the GunCon 3 of pad 1:
 *  - a shot pulls the trigger (Cross) through the core's XR input, so it fires with the gun
 *    hand and takes part in the calibration gesture (menu held + shot at the screen centre);
 *  - the magazine out holds A1 (L1): in Time Crisis 4 that is the pedal released, the player
 *    hides and the gun reloads, which is exactly what pulling the magazine means;
 *  - the charging handle taps A1 for a moment: a reload gesture for the rifle.
 */
object WonderFitterGun {
    private const val TAG = "WonderFitterGun"
    private val SERVICE: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
    private val NOTIFY: UUID = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb")
    private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /** Longest gap between two full-auto shot frames that still counts as one held trigger. */
    private const val HOLD_MS = 150L
    /** How long the charging handle's pull is stretched so the game samples it. */
    private const val BOLT_PRESS_MS = 150L
    private const val GUN_PORT = 0
    private const val RESCAN_DELAY_MS = 2000L

    val PERMISSIONS = arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)

    private val main = Handler(Looper.getMainLooper())
    private var context: Context? = null
    private var adapter: BluetoothAdapter? = null
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var lastSequence = -1
    @Volatile private var ready = false
    @Volatile var name: String = ""
        private set

    private var triggerHeld = false
    private var boltHeld = false
    private var magazineOut = false

    fun permitted(ctx: Context): Boolean =
        PERMISSIONS.all { ctx.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    /** A gun is connected and notifying. */
    fun connected(): Boolean = ready

    /** Looks for a gun and keeps it while connected; a no-op without Bluetooth or permission. */
    fun start(ctx: Context) {
        if (adapter != null) return
        if (!permitted(ctx)) {
            Log.i(TAG, "Bluetooth permissions not granted; no WonderFitter gun")
            return
        }
        val app = ctx.applicationContext
        val manager = app.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val a = manager?.adapter
        if (a == null || !a.isEnabled) {
            Log.i(TAG, "Bluetooth unavailable; no WonderFitter gun")
            return
        }
        context = app
        adapter = a
        scan()
    }

    /** Releases the gun for another app (Tremu takes it back when it is in front). */
    fun stop() {
        val a = adapter ?: return
        try {
            if (scanning) a.bluetoothLeScanner?.stopScan(scanCallback)
            gatt?.disconnect()
            gatt?.close()
        } catch (e: SecurityException) {
            Log.w(TAG, "Cannot release the gun: $e")
        }
        scanning = false
        gatt = null
        adapter = null
        context = null
        main.removeCallbacksAndMessages(null)
        setReady(false)
        // Nothing stays pressed on a gun that is gone.
        if (triggerHeld) { triggerHeld = false; trigger(false) }
        if (boltHeld) { boltHeld = false; padButton(KeyEvent.KEYCODE_BUTTON_L1, false) }
        if (magazineOut) { magazineOut = false; padButton(KeyEvent.KEYCODE_BUTTON_L1, false) }
        Log.i(TAG, "Gun released")
    }

    private fun scan() {
        val a = adapter ?: return
        if (scanning || gatt != null) return
        try {
            val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
            val scanner = a.bluetoothLeScanner ?: return
            scanner.startScan(listOf(filter), settings, scanCallback)
            scanning = true
            Log.i(TAG, "Scanning for a WonderFitter gun")
        } catch (e: SecurityException) {
            Log.w(TAG, "Cannot scan: $e")
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (gatt != null) return
            val ctx = context ?: return
            try {
                adapter?.bluetoothLeScanner?.stopScan(this)
                scanning = false
                val device: BluetoothDevice = result.device
                val n = device.name ?: result.scanRecord?.deviceName ?: device.address
                name = n.replace(Regex("[^A-Za-z0-9_-]"), "")
                Log.i(TAG, "Connecting to ${device.address} $n")
                gatt = device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } catch (e: SecurityException) {
                Log.w(TAG, "Cannot connect: $e")
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            Log.w(TAG, "Scan failed ($errorCode)")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            try {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    g.discoverServices()
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    Log.i(TAG, "Gun disconnected")
                    g.close()
                    gatt = null
                    setReady(false)
                    if (adapter != null) main.postDelayed({ scan() }, RESCAN_DELAY_MS)
                }
            } catch (e: SecurityException) {
                Log.w(TAG, "Gun connection: $e")
            }
        }

        @Suppress("DEPRECATION")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val ch = g.getService(SERVICE)?.getCharacteristic(NOTIFY)
            val cccd = ch?.getDescriptor(CCCD)
            if (ch == null || cccd == null) {
                Log.w(TAG, "Gun has no FFF1 notifications")
                return
            }
            try {
                g.setCharacteristicNotification(ch, true)
                if (android.os.Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(cccd)
                }
            } catch (e: SecurityException) {
                Log.w(TAG, "Cannot subscribe to the gun: $e")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            Log.i(TAG, "Gun '$name' ready (status $status)")
            // The gun only reports magazine changes, so it starts loaded.
            setReady(true)
        }

        // API 33+ delivers the value with the callback; the older overload reads it from the
        // characteristic. Both run on a binder thread: the parse hands off to the main thread.
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) {
            main.post { parse(value) }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (android.os.Build.VERSION.SDK_INT >= 33) return
            @Suppress("DEPRECATION")
            val value = ch.value ?: return
            main.post { parse(value) }
        }
    }

    private fun setReady(on: Boolean) {
        if (ready == on) return
        ready = on
        // A controller riding in the gun never pulls its own trigger: the hand is fixed.
        context?.let { XrPresentation.applyGunHand(it, on) }
    }

    /** Main thread. */
    private fun parse(data: ByteArray) {
        var i = 0
        while (i < data.size) {
            val type = data[i].toInt() and 0xFF
            val length = when (type) {
                0x5B -> 6
                0x5C, 0x60 -> 4
                else -> 0
            }
            if (length == 0 || i + length > data.size) { i++; continue }
            var sum = 0
            for (k in i until i + length - 2) sum += data[k].toInt() and 0xFF
            val checksum = data[i + length - 2].toInt() and 0xFF
            val end = data[i + length - 1].toInt() and 0xFF
            if ((sum and 0xFF) != checksum || end != 0x5D) { i++; continue }
            val arg = data[i + 1].toInt() and 0xFF
            when (type) {
                0x5B -> if (arg != lastSequence) { lastSequence = arg; shot() }
                0x5C -> magazine(arg != 0)
                else -> bolt(arg != 0)
            }
            i += length
        }
    }

    private val releaseTrigger = Runnable {
        triggerHeld = false
        trigger(false)
    }

    private fun shot() {
        main.removeCallbacks(releaseTrigger)
        if (!triggerHeld) {
            triggerHeld = true
            trigger(true)
        }
        main.postDelayed(releaseTrigger, HOLD_MS)
    }

    private fun magazine(inserted: Boolean) {
        Log.i(TAG, if (inserted) "Magazine in" else "Magazine out")
        val out = !inserted
        if (out == magazineOut) return
        magazineOut = out
        if (!boltHeld) padButton(KeyEvent.KEYCODE_BUTTON_L1, out)
    }

    private val releaseBolt = Runnable {
        boltHeld = false
        if (!magazineOut) padButton(KeyEvent.KEYCODE_BUTTON_L1, false)
    }

    private fun bolt(pulled: Boolean) {
        Log.i(TAG, if (pulled) "Bolt pulled" else "Bolt released")
        if (!pulled) return
        main.removeCallbacks(releaseBolt)
        if (!boltHeld) {
            boltHeld = true
            if (!magazineOut) padButton(KeyEvent.KEYCODE_BUTTON_L1, true)
        }
        main.postDelayed(releaseBolt, BOLT_PRESS_MS)
    }

    /** The trigger goes through the core's XR input when the headset session is up (it fires
     *  with the gun hand and can calibrate it); otherwise straight to the pad as Cross. */
    private fun trigger(down: Boolean) {
        val viaXr = runCatching { ARMSX3.instance.xrActive() }.getOrDefault(false)
        if (viaXr) {
            runCatching { ARMSX3.instance.xrGunTrigger(down) }
        } else {
            padButton(KeyEvent.KEYCODE_BUTTON_A, down)
        }
    }

    private fun padButton(keycode: Int, down: Boolean) {
        runCatching { Rpcs3Bridge.setPadButton(GUN_PORT, keycode, 0, down) }
    }
}
