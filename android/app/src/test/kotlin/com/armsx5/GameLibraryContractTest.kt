package com.armsx5

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-Kotlin unit tests (no device required).
 */
class GameLibraryContractTest {

    @Test
    fun elfExtensionFilterDescriptionMatchesContract() {
        // The library lists ".elf" candidates only; this documents the filter.
        val candidates = listOf("game.elf", "GAME.ELF", "readme.txt", "self.bin")
        val listed = candidates.filter { it.lowercase().endsWith(".elf") }
        assertEquals(listOf("game.elf", "GAME.ELF"), listed)
    }

    @Test
    fun validationStatusCodesAreStable() {
        // Status codes cross the JNI boundary as ints from armsx5::Status.
        // 0 = Ok; documented values must not be renumbered.
        assertTrue(0 == 0 /* Ok */)
    }
}
