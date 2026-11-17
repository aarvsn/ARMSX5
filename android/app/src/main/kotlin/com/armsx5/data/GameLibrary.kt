package com.armsx5.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File

/**
 * Game library backed by a user-selected SAF tree. Files are imported
 * (copied) into app-private storage before being handed to the native
 * validator, because the native core needs a real filesystem path.
 *
 * ARMSX5 performs no scanning of storage outside the user-picked tree and
 * never downloads or bundles game content.
 */
class GameLibrary(private val context: Context) {

    data class Entry(
        val name: String,
        val sizeBytes: Long,
        val lastModified: Long,
        val uri: Uri,
        val importedFile: File?,
    )

    var treeUri: Uri?
        get() = prefs.getString(KEY_TREE, null)?.let(Uri::parse)
        set(value) {
            prefs.edit().putString(KEY_TREE, value?.toString()).apply()
        }

    private val prefs by lazy {
        context.getSharedPreferences("armsx5_library", Context.MODE_PRIVATE)
    }

    /** Shows the SAF folder picker; call from an Activity result launcher. */
    fun pickFolderIntent() =
        android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    android.content.Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
                    android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION,
            )
        }

    fun onFolderPicked(uri: Uri) {
        context.contentResolver.takePersistableUriPermission(
            uri,
            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
        )
        treeUri = uri
    }

    /** Lists candidate executables in the picked tree (by extension filter). */
    fun listCandidates(): List<Entry> {
        val uri = treeUri ?: return emptyList()
        val tree = DocumentFile.fromTreeUri(context, uri) ?: return emptyList()
        return tree.listFiles()
            .filter { it.isFile && it.name?.lowercase()?.endsWith(".elf") == true }
            .map { Entry(it.name ?: "?", it.length(), it.lastModified(), it.uri, null) }
    }

    /**
     * Copies a candidate into private storage and returns the imported file.
     * Importing is explicit and user-initiated; nothing is executed here.
     */
    fun import(entry: Entry): File? {
        return try {
            val out = File(context.cacheDir, "imported/${entry.name}").also {
                it.parentFile?.mkdirs()
            }
            context.contentResolver.openInputStream(entry.uri)?.use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            } ?: return null
            out
        } catch (e: Exception) {
            null
        }
    }

    fun validateImported(file: File): Int = Armsx5Native.validateExecutable(file.absolutePath)

    companion object {
        private const val KEY_TREE = "tree_uri"

        /** Returns a human-readable path for a SAF tree (best effort). */
        fun describeTree(uri: Uri): String = try {
            DocumentsContract.getTreeDocumentId(uri)
        } catch (e: Exception) {
            uri.toString()
        }
    }
}
