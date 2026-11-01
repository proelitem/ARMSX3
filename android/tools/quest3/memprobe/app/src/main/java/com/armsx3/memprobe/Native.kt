package com.armsx3.memprobe

object Native {
    init {
        System.loadLibrary("memprobe")
    }

    const val MODE_ANONYMOUS = 0
    const val MODE_FILE = 1
    const val MODE_MEMFD = 2

    external fun create(mode: Int, sizeMb: Long, dir: String): Long
    external fun touch(handle: Long, fromMb: Long, toMb: Long): Long
    external fun randomRead(handle: Long, samples: Int): String
    external fun rewrite(handle: Long, hotMb: Long, seconds: Int): Long
    external fun releaseAll()
    external fun lastError(): String
    external fun procStats(): String
}
