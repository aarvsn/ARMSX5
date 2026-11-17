package com.armsx5.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx5.data.Armsx5Native

@Composable
fun SettingsScreen() {
    // Every control shown here maps to a real native implementation.
    // Anything not implemented is deliberately not exposed.
    var traceLogging by remember { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Settings", style = MaterialTheme.typography.headlineMedium)

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("CPU backend", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Interpreter — active. The interpreter is the correctness reference; " +
                        "all implemented instructions (integer + SSE subset) execute through it.",
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "ARM64 JIT — implemented for a subset of integer instructions " +
                        "(translation verified by unit tests; execution requires an " +
                        "aarch64 host). Not selectable for guest sessions yet.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("GPU backend", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Vulkan device probing is implemented (capability reporting). " +
                        "Guest GPU command processing and shader translation are in " +
                        "development — see docs/architecture/gpu.md.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Diagnostic logging", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Verbose native logging (stdout/logcat).",
                    style = MaterialTheme.typography.bodySmall,
                )
                Switch(
                    checked = traceLogging,
                    onCheckedChange = {
                        traceLogging = it
                        Armsx5Native.setLogLevel(if (it) 0 else 2)
                    },
                )
            }
        }
    }
}
