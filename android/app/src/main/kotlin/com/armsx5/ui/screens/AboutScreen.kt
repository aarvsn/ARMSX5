package com.armsx5.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx5.BuildConfig

@Composable
fun AboutScreen() {
    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("About", style = MaterialTheme.typography.headlineMedium)
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("ARMSX5 ${BuildConfig.VERSION_NAME}", style = MaterialTheme.typography.titleMedium)
                Text(
                    "An open-source research project to run legally obtained PlayStation 5 " +
                        "software on Android (ARM64-v8a), written in Kotlin and C++23 with " +
                        "Vulkan graphics.",
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "License: GPL-2.0-only. This build is a development milestone, not a " +
                        "product: commercial PS5 software does not run on it. See README.md " +
                        "and docs/compatibility/README.md for the honest status.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Upstream research & attribution", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Kyty (InoriRus, MIT) — SELF/PS5 research reference. " +
                        "shadPS4 (GPL-2.0) — PM4/GNM and x86-64 emulation reference. " +
                        "KytyPS5, LSX4, Lumen Emu — related PS5-on-desktop/Android efforts " +
                        "studied during the upstream audit (docs/research/upstream-audit.md). " +
                        "GoogleTest (BSD-3) and Khronos Vulkan headers (Apache-2.0) are vendored.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Legal", style = MaterialTheme.typography.titleMedium)
                Text(
                    "ARMSX5 contains no games, firmware, keys, or copyrighted system files, " +
                        "and implements no access-control circumvention. Users may only load " +
                        "software they legally own. Development uses original homebrew test " +
                        "binaries built in this repository.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}
