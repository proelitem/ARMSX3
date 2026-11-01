package com.armsx2

import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.os.SystemClock
import android.util.Log
import com.armsx2.runtime.MainActivityRuntime
import net.rpcsx.RPCSX

/**
 * System memory, watched for the core.
 *
 * Two jobs, both cheap:
 *  1. While a game runs, write what Android reports (available, total, the kill threshold and
 *     its lowMemory flag) into the core's log every [INTERVAL_MS], beside the kernel's own
 *     breakdown of this process. That is the measurement everything in
 *     android/tools/quest3/MEMOIRE-QUEST3.md depends on.
 *  2. Turn pressure into action before the OS does it for us. onTrimMemory from the
 *     Application, and our own reading of MemoryInfo in between (the OS does not call
 *     onTrimMemory often for a foreground app), both become a trim request the renderer
 *     applies at its next frame: moderate drops unused textures, severe also drops stale
 *     render targets. Rate-limited, since a trim that found nothing will find nothing a second
 *     later either.
 *
 * A background daemon thread rather than a coroutine on an activity: it has to outlive the
 * panel activity when the game runs in the headset's immersive activity.
 */
object MemoryWatch {
    private const val TAG = "MemoryWatch"
    private const val INTERVAL_MS = 15_000L
    private const val IDLE_MS = 2_000L
    private const val MIN_TRIM_GAP_MS = 20_000L

    @Volatile private var started = false
    @Volatile private var lastTrimAt = 0L

    fun start(context: Context) {
        if (started) return
        started = true
        val app = context.applicationContext
        Thread({ loop(app) }, "armsx3-memory-watch").apply { isDaemon = true }.start()
    }

    private fun loop(app: Context) {
        val am = app.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return
        val info = ActivityManager.MemoryInfo()
        while (true) {
            val running = runCatching { MainActivityRuntime.eState.value != EmuState.STOPPED }.getOrDefault(false)
            if (!running) {
                SystemClock.sleep(IDLE_MS)
                continue
            }
            runCatching {
                am.getMemoryInfo(info)
                val availMb = info.availMem / (1024 * 1024)
                val totalMb = info.totalMem / (1024 * 1024)
                val thresholdMb = info.threshold / (1024 * 1024)
                RPCSX.instance.memoryStatus(availMb, totalMb, thresholdMb, info.lowMemory)
                when {
                    info.lowMemory -> trim(1, "lowMemory")
                    // Inside 1.5x of the threshold the next allocation burst is the kill.
                    availMb < thresholdMb * 3 / 2 -> trim(0, "available ${availMb}MB near threshold ${thresholdMb}MB")
                }
            }.onFailure { Log.w(TAG, "memory poll failed", it) }
            SystemClock.sleep(INTERVAL_MS)
        }
    }

    /** Application.onTrimMemory. Background levels are ignored: no game is on screen then,
     *  and the renderer's caches are what the next game will rebuild anyway. */
    fun onTrimMemory(level: Int) {
        when (level) {
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL -> trim(1, "TRIM_MEMORY_RUNNING_CRITICAL")
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW -> trim(1, "TRIM_MEMORY_RUNNING_LOW")
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE -> trim(0, "TRIM_MEMORY_RUNNING_MODERATE")
            else -> Log.i(TAG, "onTrimMemory($level) while not in the foreground; ignored")
        }
    }

    private fun trim(level: Int, reason: String) {
        val now = SystemClock.elapsedRealtime()
        // A severe request always goes through; a moderate one waits out the gap.
        if (level < 1 && now - lastTrimAt < MIN_TRIM_GAP_MS) return
        lastTrimAt = now
        Log.w(TAG, "memory pressure ($reason) -> trim level $level")
        runCatching { RPCSX.instance.memoryPressure(level) }
            .onFailure { Log.w(TAG, "memoryPressure unavailable", it) }
    }
}
