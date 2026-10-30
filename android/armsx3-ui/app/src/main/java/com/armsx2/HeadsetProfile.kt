package com.armsx2

import android.os.Build
import com.armsx2.config.Settings

/**
 * Standalone VR headsets that run Android: the Meta Quest line on Horizon OS today.
 *
 * A headset is a phone-class SoC behind a compositor, a thermal budget and an OS that is
 * not stock Android, so three things the app decides per device need a different answer
 * on one:
 *
 *  1. Which Vulkan driver to recommend. The Quest 3 / 3S GPU is an Adreno 740, which the
 *     GL_RENDERER rule in [GpuInfo] maps to a Turnip pack. Whether an adrenotools Turnip
 *     loads under Horizon OS at all is unverified for this app: a comparable project
 *     (WoWForeverywhere, Oct 2026) had every Turnip it tried fail at device creation with
 *     VK_ERROR_OUT_OF_DEVICE_MEMORY on a Quest 3 and shipped the system driver there, while
 *     WinlatorXR reports Turnip working on the same headset. Until android/tools/quest3's
 *     spike has answered that on a real unit, a Quest 3 is steered to the system driver,
 *     which is the one configuration known to initialise. Flip [driverRecommendation] once
 *     the spike says which pack loads and renders.
 *
 *  2. Fresh-install defaults. Pinned, not lowered: PS3 output at 720p native, no MSAA, no
 *     GPU turbo, the two-frame queue. These are the app's defaults today, but a headset is
 *     where a later change of default towards 1080p or turbo would hurt first (640 MHz GPU
 *     under a compositor, sealed enclosure), and 720p is also what RPCS3's stereo 3D output
 *     requires, which is where the VR work is going. The VRAM ceiling is deliberately NOT
 *     lowered: see [com.armsx2.config.Ps3Settings.vramLimitMb], it is a hard VMA cap and a
 *     lower value fails allocations earlier rather than freeing anything.
 *
 *  3. Identification in the log. Build.MODEL on a Quest reads "Quest 3" with manufacturer
 *     "Oculus", which a tester's log should say in so many words, next to the SoC line.
 *
 * Detection is a pure function over Build fields so it is testable on the JVM; the
 * Build-reading entry point is [current].
 */
object HeadsetProfile {

    /** What the headset is, as far as the app needs to know. */
    enum class Model(val displayName: String, val gpu: String) {
        QUEST_3("Meta Quest 3", "Adreno 740"),
        QUEST_3S("Meta Quest 3S", "Adreno 740"),
        QUEST_PRO("Meta Quest Pro", "Adreno 650"),
        QUEST_2("Meta Quest 2", "Adreno 650"),
        /** A Quest the table above does not name: a newer one, or a first-generation unit
         *  (which cannot install this app anyway, it runs Android 10). Treated as a Quest
         *  for defaults and logging, with no driver opinion. */
        QUEST_OTHER("Meta Quest", "Adreno"),
    }

    data class Headset(val model: Model, val rawModel: String, val rawDevice: String) {
        /** Snapdragon XR2 Gen 2 parts: the Adreno 740 at 640 MHz, and the units the VR
         *  work targets. */
        val isXr2Gen2: Boolean get() = model == Model.QUEST_3 || model == Model.QUEST_3S
        val displayName: String get() = model.displayName
    }

    /** Horizon OS reports the platform codename in Build.DEVICE. The MODEL string is the
     *  primary key; the codename is the fallback for a build that renames the model. */
    private val deviceCodenames = mapOf(
        "eureka" to Model.QUEST_3,
        "panther" to Model.QUEST_3S,
        "seacliff" to Model.QUEST_PRO,
        "hollywood" to Model.QUEST_2,
    )

    /**
     * Pure classifier over the Build fields. Returns null for anything that is not a Meta
     * headset. Manufacturer "Oculus" is what Horizon OS reports today; "Meta" is accepted
     * for the day it changes. A model string starting with "Quest" is accepted on its own,
     * since that is the one fact every Quest reports.
     */
    fun detect(manufacturer: String?, brand: String?, model: String?, device: String?): Headset? {
        val m = model?.trim().orEmpty()
        val d = device?.trim().orEmpty().lowercase()
        val metaVendor = listOf(manufacturer, brand).any {
            val v = it?.trim().orEmpty().lowercase()
            v == "oculus" || v == "meta"
        }
        val questModel = m.startsWith("Quest", ignoreCase = true)
        val knownCodename = deviceCodenames[d]

        if (!metaVendor && !questModel && knownCodename == null) return null
        // A Meta vendor string on a non-Quest product (Ray-Ban glasses report "Meta" too) is
        // not a headset.
        if (!questModel && knownCodename == null) return null

        val normalized = m.lowercase().replace(Regex("""\s+"""), " ")
        val byModel = when {
            normalized == "quest 3s" -> Model.QUEST_3S
            normalized == "quest 3" -> Model.QUEST_3
            normalized == "quest pro" -> Model.QUEST_PRO
            normalized == "quest 2" -> Model.QUEST_2
            else -> null
        }
        return Headset(byModel ?: knownCodename ?: Model.QUEST_OTHER, m, d)
    }

    /** The headset this process runs on, or null on a phone or tablet. Read once. */
    val current: Headset? by lazy {
        runCatching { detect(Build.MANUFACTURER, Build.BRAND, Build.MODEL, Build.DEVICE) }.getOrNull()
    }

    fun isHeadset(): Boolean = current != null

    /**
     * Driver source to recommend on [headset], or null to let the GL_RENDERER rule decide.
     *
     * Quest 3 / 3S: the system driver, for the reason given in the class comment. The reason
     * text is shown verbatim under the recommendation, as the other rules' reasons are.
     *
     * Quest 2 / Pro: null. Their Adreno 650 falls under the Adreno 6xx rule, and Turnip on a
     * Quest 2 is established practice in the Winlator community.
     */
    fun driverRecommendation(headset: Headset): GpuInfo.Recommendation? = when (headset.model) {
        Model.QUEST_3, Model.QUEST_3S -> GpuInfo.Recommendation(
            sourceLabel = "",
            reason = "${headset.displayName}: custom Turnip packs are not yet verified to load on " +
                "Horizon OS. The system driver is the one known to initialise here; a Turnip can " +
                "still be installed and tried from the list below.",
            systemDriver = true,
        )
        else -> null
    }

    /**
     * The global settings a fresh install on [headset] starts from. [base] unchanged when
     * [headset] is null. Pins, never lowers; see the class comment for what and why.
     */
    fun freshInstallDefaults(headset: Headset?, base: Settings): Settings {
        if (headset == null) return base
        return base.copy(
            ps3 = base.ps3.copy(
                resolution = 2,        // 1280x720, the PS3's native output and what stereo 3D needs
                resolutionScale = 100, // no internal upscale on a 640 MHz GPU under a compositor
                msaaMode = 0,
                gpuTurbo = false,      // KGSL max clocks in a sealed enclosure: the user's call, never a default
            ),
            vsyncQueueSize = 2,        // the smoother two-frame queue, not low-latency mode
        )
    }

    /** "<soc> · Meta Quest 3 (eureka)" for the core's SoC log line, or [soc] untouched on a
     *  phone. Appended rather than replaced so the Qualcomm part number stays visible. */
    fun annotateSocIdentity(headset: Headset?, soc: String): String {
        if (headset == null) return soc
        val tag = buildString {
            append(headset.displayName)
            if (headset.rawDevice.isNotBlank()) append(" (").append(headset.rawDevice).append(")")
        }
        return if (soc.isBlank()) tag else "$soc · $tag"
    }
}
