package com.armsx5.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.armsx5.data.Armsx5Native

@Composable
fun DiagnosticsScreen() {
    val context = LocalContext.current
    var deviceJson by remember { mutableStateOf("probing...") }
    var testReport by remember { mutableStateOf("") }
    var running by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }

    LaunchedEffect(Unit) { deviceJson = Armsx5Native.deviceReportJson() }

    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Diagnostics", style = MaterialTheme.typography.headlineMedium)

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Device capabilities", style = MaterialTheme.typography.titleMedium)
                Text(deviceJson, style = MaterialTheme.typography.bodySmall)
            }
        }

        Button(
            enabled = !running,
            onClick = {
                running = true
                error = ""
                testReport = ""
                try {
                    val elf = Armsx5Native.copyAssetToCache(context, "hello.elf")
                    if (elf == null) {
                        error = "Built-in test ELF missing from assets."
                    } else {
                        testReport = Armsx5Native.runTestExecutable(elf.absolutePath)
                    }
                } finally {
                    running = false
                }
            },
        ) { Text(if (running) "Running..." else "Run built-in homebrew test") }

        if (error.isNotEmpty()) Text(error, color = MaterialTheme.colorScheme.error)
        if (testReport.isNotEmpty()) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("Built-in test session", style = MaterialTheme.typography.titleMedium)
                    Text(testReport, style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("What this proves", style = MaterialTheme.typography.titleMedium)
                Text(
                    "The built-in test is an original x86-64 ELF compiled with no host " +
                        "libraries. Running it exercises: ELF64 parsing, guest address-space " +
                        "mapping, instruction decoding, the interpreter, FreeBSD-style " +
                        "syscall emulation (write/exit), and guest stack handling.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}
