package com.armsx2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HeadsetProfileTest {
    // Build fields as Horizon OS reports them: manufacturer "Oculus", brand "oculus",
    // model "Quest 3", device codename "eureka".
    private fun quest3() = HeadsetProfile.detect("Oculus", "oculus", "Quest 3", "eureka")

    @Test
    fun quest3IsDetectedFromModel() {
        val h = quest3()
        assertEquals(HeadsetProfile.Model.QUEST_3, h?.model)
        assertTrue(h!!.isXr2Gen2)
        assertEquals("Meta Quest 3", h.displayName)
    }

    @Test
    fun quest3sIsNotMistakenForQuest3() {
        val h = HeadsetProfile.detect("Oculus", "oculus", "Quest 3S", "panther")
        assertEquals(HeadsetProfile.Model.QUEST_3S, h?.model)
        assertTrue(h!!.isXr2Gen2)
    }

    @Test
    fun quest2AndProAreGen1() {
        assertEquals(HeadsetProfile.Model.QUEST_2, HeadsetProfile.detect("Oculus", "oculus", "Quest 2", "hollywood")?.model)
        assertEquals(HeadsetProfile.Model.QUEST_PRO, HeadsetProfile.detect("Oculus", "oculus", "Quest Pro", "seacliff")?.model)
        assertFalse(HeadsetProfile.detect("Oculus", "oculus", "Quest 2", "hollywood")!!.isXr2Gen2)
    }

    @Test
    fun codenameIdentifiesARenamedModel() {
        // A future build that reports a marketing name instead of "Quest 3".
        assertEquals(HeadsetProfile.Model.QUEST_3, HeadsetProfile.detect("Meta", "meta", "Meta Horizon Headset", "eureka")?.model)
    }

    @Test
    fun unknownQuestStillCountsAsAQuest() {
        val h = HeadsetProfile.detect("Oculus", "oculus", "Quest 4", "newcodename")
        assertEquals(HeadsetProfile.Model.QUEST_OTHER, h?.model)
        assertEquals("Quest 4", h?.rawModel)
    }

    @Test
    fun phonesAndGlassesAreNotHeadsets() {
        assertNull(HeadsetProfile.detect("samsung", "samsung", "SM-S918B", "dm3q"))
        assertNull(HeadsetProfile.detect("AYN", "AYN", "Odin 3", "odin3"))
        // Meta as a vendor on a non-Quest product.
        assertNull(HeadsetProfile.detect("Meta", "meta", "Ray-Ban Meta", "glasses"))
        assertNull(HeadsetProfile.detect(null, null, null, null))
    }

    @Test
    fun quest3IsSteeredToTheSystemDriver() {
        val rec = GpuInfo.recommendation("Adreno (TM) 740", quest3())
        assertTrue(rec!!.systemDriver)
        assertTrue(rec.reason.contains("Quest 3"))
    }

    @Test
    fun thePhoneRuleStillAppliesWithoutAHeadset() {
        val rec = GpuInfo.recommendation("Adreno (TM) 740", null)
        assertFalse(rec!!.systemDriver)
        assertEquals("MrPurple · purple-turnip", rec.sourceLabel)
    }

    @Test
    fun quest2FallsThroughToTheAdreno6xxRule() {
        val rec = GpuInfo.recommendation("Adreno (TM) 650", HeadsetProfile.detect("Oculus", "oculus", "Quest 2", "hollywood"))
        assertFalse(rec!!.systemDriver)
        assertEquals("AdrenoToolsDrivers", rec.sourceLabel)
    }

    @Test
    fun socLineNamesTheHeadset() {
        assertEquals(
            "Qualcomm SXR2230P (Snapdragon XR2 Gen 2) · Meta Quest 3 (eureka)",
            HeadsetProfile.annotateSocIdentity(quest3(), "Qualcomm SXR2230P (Snapdragon XR2 Gen 2)"),
        )
        assertEquals("Meta Quest 3 (eureka)", HeadsetProfile.annotateSocIdentity(quest3(), ""))
        assertEquals("Qualcomm SM8550", HeadsetProfile.annotateSocIdentity(null, "Qualcomm SM8550"))
    }
}
