package com.armsx5.data

import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File

/**
 * JNI bridge to the native ARMSX5 runtime. All heavy work happens in C++23;
 * this object only marshals data across the boundary.
 */
object Armsx5Native {

    const val NATIVE_VERSION = "0.2.0"

    @Volatile
    private var initialized = false

    @Volatile
    var cacheDirPath: String? = null

    fun loadLibrary() {
        System.loadLibrary("armsx5_jni")
    }

    fun init(configJson: String? = null): Boolean {
        if (initialized) return true
        val ok = nativeInit(configJson ?: defaultConfigJson())
        initialized = ok
        return ok
    }

    fun shutdown() {
        if (initialized) {
            nativeShutdown()
            initialized = false
        }
    }

    fun isInitialized(): Boolean = initialized

    private fun defaultConfigJson(): String {
        val dataDir = File(cacheDirPath ?: "").parentFile?.absolutePath ?: ""
        return """
            {"log_level": 2,
             "cpu_backend": "interpreter",
             "jit_enabled": false,
             "data_dir": "$dataDir"}
        """.trimIndent()
    }

    // ---- device reporting (Kotlin supplies Build.* facts the NDK cannot see) ----
    fun deviceReportJson(): String {
        val native = try {
            nativeDeviceInfoJson()
        } catch (e: UnsatisfiedLinkError) {
            Log.w("Armsx5", "native device info unavailable", e)
            "{}"
        }
        val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MODEL else "unknown"
        return """{"soc":"$soc",
                    "board":"${Build.BOARD}",
                    "manufacturer":"${Build.MANUFACTURER}",
                    "model":"${Build.MODEL}",
                    "android_release":"${Build.VERSION.RELEASE}",
                    "supported_abis":${Build.SUPPORTED_ABIS.joinToString(",", "[", "]") { "\"$it\"" }},
                    "native":$native}"""
    }

    // ---- executable validation / execution (plain ELF64 only) ----
    fun validateExecutable(path: String): Int = nativeValidateExecutable(path)

    fun executableInfoJson(path: String): String = nativeExecutableInfoJson(path)

    fun runTestExecutable(path: String, maxSteps: Long = 100_000_000L): String =
        nativeRunTestExecutable(path, maxSteps)

    fun copyAssetToCache(context: Context, assetName: String): File? {
        return try {
            val out = File(context.cacheDir, assetName)
            context.assets.open(assetName).use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            }
            out
        } catch (e: Exception) {
            Log.w("Armsx5", "asset copy failed: $assetName", e)
            null
        }
    }

    // ---- controller input (events forwarded into the native aggregator) ----
    fun onControllerButton(keycode: Int, pressed: Boolean) =
        nativeOnControllerButton(keycode, pressed)

    fun onControllerAxis(axis: Int, value: Float) = nativeOnControllerAxis(axis, value)

    fun controllerStateJson(): String = nativeControllerStateJson()

    fun setLogLevel(level: Int) = nativeSetLogLevel(level)

    // ---- native bindings ----
    private external fun nativeInit(configJson: String): Boolean
    private external fun nativeShutdown(): Boolean
    private external fun nativeDeviceInfoJson(): String
    private external fun nativeValidateExecutable(path: String): Int
    private external fun nativeExecutableInfoJson(path: String): String
    private external fun nativeRunTestExecutable(path: String, maxSteps: Long): String
    private external fun nativeOnControllerButton(keycode: Int, pressed: Boolean)
    private external fun nativeOnControllerAxis(axis: Int, value: Float)
    private external fun nativeControllerStateJson(): String
    private external fun nativeSetLogLevel(level: Int)
}
