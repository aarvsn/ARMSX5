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
import java.io.File

@Composable
fun GameDetailsScreen(name: String) {
    val context = LocalContext.current
    var info by remember { mutableStateOf("not validated yet") }
    var runReport by remember { mutableStateOf("") }
    var running by remember { mutableStateOf(false) }

    val imported = remember(name) { File(context.cacheDir, "imported/$name") }

    LaunchedEffect(name) {
        if (imported.exists() && imported.length() > 0) {
            val code = Armsx5Native.validateExecutable(imported.absolutePath)
            info = if (code == 0) {
                Armsx5Native.executableInfoJson(imported.absolutePath)
            } else {
                "Validation failed (status $code). Plain ELF64 x86-64 executables only."
            }
        } else {
            info = "File not imported yet — validate it from the Library tab first."
        }
    }

    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(name, style = MaterialTheme.typography.headlineSmall)
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Executable metadata", style = MaterialTheme.typography.titleMedium)
                Text(info, style = MaterialTheme.typography.bodySmall)
            }
        }
        Button(
            enabled = !running && imported.exists(),
            onClick = {
                running = true
                runReport = ""
                try {
                    runReport = Armsx5Native.runTestExecutable(imported.absolutePath)
                } finally {
                    running = false
                }
            },
        ) { Text(if (running) "Running..." else "Run test executable") }
        if (runReport.isNotEmpty()) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text("Session report", style = MaterialTheme.typography.titleMedium)
                    Text(runReport, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}
