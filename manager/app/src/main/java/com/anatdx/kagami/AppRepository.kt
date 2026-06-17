package com.anatdx.kagami

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.graphics.drawable.Drawable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

data class AppEntry(
    val packageName: String,
    val label: String,
    val uid: Int,
    val isSystem: Boolean,
    val sourceDir: String,
    val icon: Drawable?,
)

class AppRepository(private val context: Context) {
    suspend fun loadInstalledApps(): List<AppEntry> = withContext(Dispatchers.IO) {
        val pm = context.packageManager
        val flag = PackageManager.GET_META_DATA
        val apps = pm.getInstalledApplications(flag)
        apps.asSequence()
            .filter { it.uid > 0 }
            .map { info -> info.toEntry(pm) }
            .sortedWith(compareBy<AppEntry> { it.isSystem }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.label })
            .toList()
    }

    private fun ApplicationInfo.toEntry(pm: PackageManager): AppEntry {
        val label = runCatching { pm.getApplicationLabel(this).toString() }.getOrElse { packageName }
        val icon = runCatching { pm.getApplicationIcon(this) }.getOrNull()
        val system = (flags and ApplicationInfo.FLAG_SYSTEM) != 0 ||
            (flags and ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0
        return AppEntry(
            packageName = packageName,
            label = label,
            uid = uid,
            isSystem = system,
            sourceDir = sourceDir ?: "",
            icon = icon,
        )
    }
}
