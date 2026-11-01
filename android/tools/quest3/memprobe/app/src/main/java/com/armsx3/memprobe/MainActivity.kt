package com.armsx3.memprobe

import android.app.Activity
import android.app.ActivityManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.StatFs
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread

/**
 * memprobe. Three experiments, each a button, each written line by line to
 * <externalFilesDir>/memprobe.log as it runs (the interesting end of a run is the OS killing
 * the process, and the last line written is the answer):
 *
 *  ANON   anonymous memory in 256 MB steps until killed. Where Horizon OS stops a foreground
 *         app, and whether zram takes any of it (VmSwap, pswpout).
 *  FILE   a MAP_SHARED file in app storage, in 256 MB steps to 12 GB (or what the flash has
 *         room for), then random-read latency, then 60 s of rewriting a 1 GB hot set while
 *         counting what the kernel writes back. Whether file pages count against the app's
 *         limit, what a refault costs, and what the flash would endure.
 *  MEMFD  shmem, what RPCS3 backs guest memory with: like ANON, but shared.
 *
 * After a kill, relaunch: the previous log's tail is shown first.
 */
class MainActivity : Activity() {

    private lateinit var out: TextView
    private lateinit var scroll: ScrollView
    private lateinit var logFile: File
    @Volatile private var running = false

    private val stepMb = 256L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        logFile = File(getExternalFilesDir(null) ?: filesDir, "memprobe.log")

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            setPadding(24, 24, 24, 24)
        }
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        fun button(label: String, action: () -> Unit) = Button(this).apply {
            text = label
            setOnClickListener { if (!running) action() else append("(déjà en cours)") }
            buttons.addView(this, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        }
        button("ANON jusqu'au kill") { run("anon") { runFill(Native.MODE_ANONYMOUS, 14_000) } }
        button("FILE 12 Go + réécriture") { run("file") { runFile() } }
        button("MEMFD jusqu'au kill") { run("memfd") { runFill(Native.MODE_MEMFD, 14_000) } }
        button("Libérer") { Native.releaseAll(); append("régions libérées"); append(Native.procStats()) }
        root.addView(buttons)

        out = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 11f
            setTextColor(Color.rgb(200, 230, 200))
            setTextIsSelectable(true)
        }
        scroll = ScrollView(this).apply { addView(out) }
        root.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(root)

        showPreviousTail()
        header()
    }

    private fun showPreviousTail() {
        if (!logFile.exists()) return
        val lines = runCatching { logFile.readLines() }.getOrDefault(emptyList())
        if (lines.isEmpty()) return
        out.append("=== fin du journal précédent (${lines.size} lignes) ===\n")
        lines.takeLast(25).forEach { out.append("$it\n") }
        out.append("=== nouvelle session ===\n")
    }

    private fun header() {
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also(am::getMemoryInfo)
        val fs = StatFs(logFile.parentFile!!.absolutePath)
        append("memprobe 0.1 | ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}) | Android ${Build.VERSION.RELEASE} | build ${Build.DISPLAY}")
        append("totalMem=${mi.totalMem / MB}MB availMem=${mi.availMem / MB}MB threshold=${mi.threshold / MB}MB lowMemory=${mi.lowMemory} " +
            "memoryClass=${am.memoryClass}MB largeMemoryClass=${am.largeMemoryClass}MB lowRamDevice=${am.isLowRamDevice}")
        append("stockage app: libre ${fs.availableBytes / MB}MB sur ${fs.totalBytes / MB}MB (${logFile.parent})")
        append(Native.procStats())
    }

    private fun run(name: String, body: () -> Unit) {
        running = true
        append("--- $name ---")
        thread(name = "memprobe-$name") {
            try {
                body()
            } catch (t: Throwable) {
                append("EXCEPTION ${t.javaClass.simpleName}: ${t.message}")
            } finally {
                running = false
                append("--- fin $name ---")
            }
        }
    }

    /** Grow a region by [stepMb] until the OS stops us or [maxMb] is reached. */
    private fun runFill(mode: Int, maxMb: Long): Long? {
        val dir = logFile.parentFile!!.absolutePath
        val handle = Native.create(mode, maxMb, dir)
        if (handle < 0) {
            append("create a échoué: ${Native.lastError()}")
            return null
        }
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo()
        var filled = 0L
        while (filled < maxMb) {
            val next = minOf(filled + stepMb, maxMb)
            val us = Native.touch(handle, filled, next)
            filled = next
            am.getMemoryInfo(mi)
            // Each line survives the kill that may follow it: that is the whole point.
            append("filled=${filled}MB step=${us / 1000}ms avail=${mi.availMem / MB}MB low=${mi.lowMemory} | ${Native.procStats()}")
            Thread.sleep(150)
        }
        append("rempli ${filled}MB sans être tué")
        append(Native.randomRead(handle, 2000))
        return handle
    }

    private fun runFile() {
        val fs = StatFs(logFile.parentFile!!.absolutePath)
        // Leave 2 GB of flash alone.
        val target = minOf(12_288L, fs.availableBytes / MB - 2048)
        if (target < 1024) {
            append("pas assez de stockage libre pour l'essai fichier (${fs.availableBytes / MB}MB)")
            return
        }
        append("cible fichier: ${target}MB")
        val handle = runFill(Native.MODE_FILE, target) ?: return

        // What a game does: keep rewriting a hot set. The vmstat delta says what reached flash.
        val before = Native.procStats()
        append("réécriture 1024MB pendant 60 s | avant: $before")
        val passes = Native.rewrite(handle, 1024, 60)
        val after = Native.procStats()
        append("réécriture terminée: $passes passes (${passes * 1024 / 60}MB/s dirtied) | après: $after")
        append("lecture: nr_written(après) - nr_written(avant) = MB réellement écrits sur la flash en 60 s")
        append(Native.randomRead(handle, 2000))
    }

    private val stamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    private fun append(line: String) {
        val text = "${stamp.format(Date())} $line"
        runCatching {
            FileOutputStream(logFile, true).use { it.write((text + "\n").toByteArray()); it.fd.sync() }
        }
        runOnUiThread {
            out.append("$text\n")
            scroll.post { scroll.fullScroll(View.FOCUS_DOWN) }
        }
    }

    companion object {
        private const val MB = 1024L * 1024L
    }
}
