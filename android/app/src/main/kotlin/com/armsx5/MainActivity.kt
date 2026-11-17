package com.armsx5

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Settings
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.armsx5.data.Armsx5Native
import com.armsx5.data.GameLibrary
import com.armsx5.ui.screens.AboutScreen
import com.armsx5.ui.screens.DiagnosticsScreen
import com.armsx5.ui.screens.GameDetailsScreen
import com.armsx5.ui.screens.HomeScreen
import com.armsx5.ui.screens.LibraryScreen
import com.armsx5.ui.screens.SettingsScreen
import com.armsx5.ui.theme.Armsx5Theme

class MainActivity : ComponentActivity() {

    lateinit var library: GameLibrary

    private val folderPicker =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val uri = result.data?.data ?: return@registerForActivityResult
            result.data?.flags?.and(
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )?.let { flags ->
                if (flags != 0) library.onFolderPicked(uri)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        library = GameLibrary(this)
        Armsx5Native.cacheDirPath = cacheDir.absolutePath
        Armsx5Native.loadLibrary()
        Armsx5Native.init()

        setContent {
            Armsx5Theme {
                Armsx5App(
                    onPickFolder = { folderPicker.launch(library.pickFolderIntent()) },
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) Armsx5Native.shutdown()
    }

    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        // Forward gamepad analog input into the native aggregator.
        when (event.action) {
            android.view.MotionEvent.ACTION_MOVE -> {
                Armsx5Native.onControllerAxis(0, event.getAxisValue(android.view.MotionEvent.AXIS_X))
                Armsx5Native.onControllerAxis(1, event.getAxisValue(android.view.MotionEvent.AXIS_Y))
                Armsx5Native.onControllerAxis(11, event.getAxisValue(android.view.MotionEvent.AXIS_Z))
                Armsx5Native.onControllerAxis(14, event.getAxisValue(android.view.MotionEvent.AXIS_RX))
                Armsx5Native.onControllerAxis(23, event.getAxisValue(android.view.MotionEvent.AXIS_BRAKE))
                Armsx5Native.onControllerAxis(22, event.getAxisValue(android.view.MotionEvent.AXIS_GAS))
            }
        }
        return super.dispatchGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (event.repeatCount == 0) {
            Armsx5Native.onControllerButton(event.keyCode, event.action == android.view.KeyEvent.ACTION_DOWN)
        }
        return super.dispatchKeyEvent(event)
    }
}

private data class Destination(val route: String, val label: String, val icon: ImageVector)

@Composable
fun Armsx5App(onPickFolder: () -> Unit) {
    val navController = rememberNavController()
    val destinations = listOf(
        Destination("home", "Home", Icons.Filled.Home),
        Destination("library", "Library", Icons.Filled.List),
        Destination("settings", "Settings", Icons.Filled.Settings),
        Destination("diagnostics", "Diagnostics", Icons.Filled.Info),
    )

    Scaffold(
        bottomBar = {
            NavigationBar {
                val entry by navController.currentBackStackEntryAsState()
                val current = entry?.destination
                destinations.forEach { dest ->
                    NavigationBarItem(
                        selected = current?.hierarchy?.any { it.route == dest.route } == true,
                        onClick = {
                            navController.navigate(dest.route) {
                                popUpTo("home") { saveState = true }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                        icon = { Icon(dest.icon, contentDescription = dest.label) },
                        label = { Text(dest.label) },
                    )
                }
            }
        },
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = "home",
            modifier = Modifier.padding(padding),
        ) {
            composable("home") { HomeScreen() }
            composable("library") { LibraryScreen(onPickFolder) }
            composable("details/{name}") { backStackEntry ->
                GameDetailsScreen(backStackEntry.arguments?.getString("name") ?: "")
            }
            composable("settings") { SettingsScreen() }
            composable("diagnostics") { DiagnosticsScreen() }
            composable("about") { AboutScreen() }
        }
    }
}
