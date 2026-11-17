package com.armsx5.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

// Dark theme by default per the ARMSX5 design brief: minimal surface,
// clear typography, no decorative gradients.
private val DarkColors = darkColorScheme()

@Composable
fun Armsx5Theme(content: @Composable () -> Unit) {
    // The brief pins dark-by-default; light appearance is intentionally not
    // provided until a reviewed palette exists.
    MaterialTheme(colorScheme = DarkColors, content = content)
}
