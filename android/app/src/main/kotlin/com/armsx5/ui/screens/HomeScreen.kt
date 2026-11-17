package com.armsx5.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx5.BuildConfig
import com.armsx5.data.Armsx5Native

@Composable
fun HomeScreen() {
    var deviceReport by remember { mutableStateOf("probing...") }
    var ready by remember { mutableStateOf(Armsx5Native.isInitialized()) }

    LaunchedEffect(Unit) {
        deviceReport = Armsx5Native.deviceReportJson()
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("ARMSX5", style = MaterialTheme.typography.headlineMedium)
        Text(
            "Android Runtime and Machine-System eXecution for PlayStation 5",
            style = MaterialTheme.typography.bodySmall,
        )

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Runtime status", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (ready) "Ready — interpreter backend active"
                    else "Native runtime not initialized",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    "CPU backend: interpreter (JIT is aarch64-only and experimental)",
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "GPU backend: Vulkan probe (device selection only — no renderer yet)",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Detected device", style = MaterialTheme.typography.titleMedium)
                Text(deviceReport, style = MaterialTheme.typography.bodySmall)
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Last session", style = MaterialTheme.typography.titleMedium)
                Text(
                    "No guest sessions yet. Use the Diagnostics tab to run the built-in " +
                        "homebrew test executable, or Library to import a plain ELF64 test binary.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("version ${BuildConfig.VERSION_NAME}", style = MaterialTheme.typography.labelSmall)
            Text("native ${Armsx5Native.NATIVE_VERSION}", style = MaterialTheme.typography.labelSmall)
        }
    }
}
