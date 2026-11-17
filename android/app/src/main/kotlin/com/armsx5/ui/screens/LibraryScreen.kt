package com.armsx5.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import androidx.navigation.compose.rememberNavController
import com.armsx5.data.Armsx5Native
import com.armsx5.data.GameLibrary

@Composable
fun LibraryScreen(onPickFolder: () -> Unit) {
    val context = LocalContext.current
    val navController = rememberNavController()
    val library = remember { GameLibrary(context) }
    var entries by remember { mutableStateOf(listOf<GameLibrary.Entry>()) }
    var status by remember { mutableStateOf("") }

    LaunchedEffect(library.treeUri) {
        entries = library.listCandidates()
        status = library.treeUri?.let {
            "Imported tree: ${GameLibrary.describeTree(it)}"
        } ?: "No folder selected."
    }

    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Library", style = MaterialTheme.typography.headlineMedium)
        Text(
            "Pick a folder containing plain ELF64 x86-64 test executables. " +
                "Encrypted PS5 SELF files are detected and reported as unsupported.",
            style = MaterialTheme.typography.bodySmall,
        )
        Button(onClick = onPickFolder) { Text("Import game folder") }
        Text(status, style = MaterialTheme.typography.bodySmall)

        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(entries, key = { it.name }) { entry ->
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp)) {
                        Text(entry.name, style = MaterialTheme.typography.titleSmall)
                        Text(
                            "${entry.sizeBytes} bytes",
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Button(
                            onClick = {
                                val imported = library.import(entry)
                                if (imported == null) {
                                    status = "Import failed for ${entry.name}"
                                } else {
                                    val code = library.validateImported(imported)
                                    status = "${entry.name}: validation status code $code" +
                                        " (0 = OK)"
                                    navController.navigate("details/${entry.name}")
                                }
                            },
                            enabled = true,
                        ) { Text("Validate") }
                    }
                }
            }
        }
    }
}
