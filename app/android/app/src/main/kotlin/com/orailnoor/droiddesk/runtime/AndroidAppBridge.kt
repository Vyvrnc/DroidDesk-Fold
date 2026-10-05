package com.orailnoor.droiddesk.runtime

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.Uri
import android.provider.Settings
import android.util.Log
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import java.io.File
import kotlin.concurrent.thread

/**
 * Makes Android launcher apps available to the Linux desktop without granting
 * the Linux processes any extra Android permissions.  XFCE entries connect to
 * this app-private Unix socket; the Android service then launches the chosen
 * activity through the normal Android API.
 */
object AndroidAppBridge {
    private const val TAG = "AndroidAppBridge"
    private const val SOCKET_NAME = "droiddesk.android-app-launcher"

    @Volatile private var server: LocalServerSocket? = null

    fun listApps(context: Context): List<Map<String, String>> = launcherActivities(context).map {
        mapOf(
            "label" to it.loadLabel(context.packageManager).toString(),
            "package" to it.activityInfo.packageName,
            "source" to "Android",
        )
    }

    fun launchAndroidPackage(context: Context, packageName: String): Boolean =
        launchPackage(context, packageName)

    fun getDockPackages(context: Context): List<String> {
        val preferences = context.getSharedPreferences("desktop_integration", Context.MODE_PRIVATE)
        if (preferences.contains("dock_packages")) {
            return preferences.getString("dock_packages", "").orEmpty()
                .split(',').filter(String::isNotBlank)
        }
        return defaultDockPackages(context, launcherActivities(context))
    }

    fun setDockPackages(context: Context, packages: List<String>) {
        val installed = launcherActivities(context).map { it.activityInfo.packageName }.toSet()
        val safe = packages.filter { it in installed }.distinct().take(8)
        context.getSharedPreferences("desktop_integration", Context.MODE_PRIVATE)
            .edit().putString("dock_packages", safe.joinToString(",")).commit()
    }

    fun start(context: Context) {
        if (server != null) return
        synchronized(this) {
            if (server != null) return
            try {
                val socket = LocalServerSocket(SOCKET_NAME)
                server = socket
                thread(name = "android-app-bridge", isDaemon = true) {
                    serve(context.applicationContext, socket)
                }
                Log.i(TAG, "Android app launcher bridge started")
            } catch (error: Exception) {
                Log.e(TAG, "Could not start Android app launcher bridge", error)
            }
        }
    }

    fun stop() {
        synchronized(this) {
            runCatching { server?.close() }
            server = null
        }
    }

    fun syncLaunchers(context: Context, homeDir: File, python: File, sessionRoot: File? = null) {
        if (!python.canExecute()) {
            Log.w(TAG, "Python is unavailable; Android app launchers were not synced")
            return
        }
        val appsDir = File(homeDir, ".local/share/applications/droiddesk-android").apply { mkdirs() }
        appsDir.listFiles()?.forEach { it.delete() }
        val iconsDir = File(homeDir, ".local/share/icons/droiddesk-android").apply { mkdirs() }
        iconsDir.listFiles()?.forEach { it.delete() }

        val launcher = File(homeDir, ".local/bin/droiddesk-launch-android-app.py")
        fun sessionPath(file: File): String = if (sessionRoot == null) {
            file.absolutePath
        } else {
            "/" + file.relativeTo(sessionRoot).invariantSeparatorsPath
        }
        val pythonPath = sessionPath(python)
        val launcherPath = sessionPath(launcher)
        launcher.parentFile?.mkdirs()
        launcher.writeText(
            """
            #!$pythonPath
            import socket
            import sys

            if len(sys.argv) != 2:
                raise SystemExit("Expected an Android package name")
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.connect("\0$SOCKET_NAME")
            client.sendall((sys.argv[1] + "\n").encode())
            """.trimIndent() + "\n",
        )
        launcher.setExecutable(true, false)

        val activities = launcherActivities(context)

        activities.forEach { activity ->
            val packageName = activity.activityInfo.packageName
            val label = desktopEscape(activity.loadLabel(context.packageManager).toString())
            val safeName = packageName.replace(Regex("[^A-Za-z0-9_.-]"), "_")
            val iconFile = File(iconsDir, "$safeName.png")
            val icon = if (writeIconPng(activity.loadIcon(context.packageManager), iconFile)) {
                sessionPath(iconFile)
            } else {
                "applications-other"
            }
            val filename = "$safeName.desktop"
            File(appsDir, filename).writeText(
                """
                [Desktop Entry]
                Version=1.0
                Type=Application
                Name=$label
                Comment=Open Android app: $packageName
                Exec=$pythonPath $launcherPath $packageName
                Icon=$icon
                Terminal=false
                Categories=X-DroidDesk-Android;
                StartupNotify=false
                """.trimIndent() + "\n",
            )
        }
        installAndroidMenu(homeDir)
        syncUtilityLaunchers(homeDir, pythonPath, launcherPath, sessionPath(homeDir))
        syncDockLaunchers(context, homeDir, activities, appsDir)
        Log.i(TAG, "Synced ${activities.size} Android app launchers into ${appsDir.absolutePath}")
    }

    /**
     * All installed Android apps (often 150+) used to land in Accessories as
     * Utility. They get their own category and an "Android" submenu instead; the
     * user menu file merges the system one, so nothing else changes. A menu file
     * the user wrote themselves (without our marker) is left alone.
     */
    private fun installAndroidMenu(homeDir: File) {
        val marker = "<!-- DroidDesk: Android apps submenu -->"
        val menuFile = File(homeDir, ".config/menus/xfce-applications.menu")
        if (menuFile.exists() && !menuFile.readText().contains(marker)) return
        menuFile.parentFile?.mkdirs()
        menuFile.writeText(
            """
            <!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN"
              "http://www.freedesktop.org/standards/menu-spec/1.0/menu.dtd">
            $marker
            <Menu>
              <Name>Xfce</Name>
              <MergeFile type="parent"/>
              <Menu>
                <Name>Android</Name>
                <Directory>droiddesk-android.directory</Directory>
                <Include><Category>X-DroidDesk-Android</Category></Include>
              </Menu>
            </Menu>
            """.trimIndent() + "\n",
        )
        File(homeDir, ".local/share/desktop-directories").mkdirs()
        File(homeDir, ".local/share/desktop-directories/droiddesk-android.directory").writeText(
            """
            [Desktop Entry]
            Type=Directory
            Name=Android
            Icon=phone
            """.trimIndent() + "\n",
        )
    }

    private fun syncUtilityLaunchers(
        homeDir: File,
        pythonPath: String,
        launcherPath: String,
        homePath: String,
    ) {
        val appsDir = File(homeDir, ".local/share/applications/droiddesk-tools").apply {
            mkdirs()
            listFiles()?.forEach { it.delete() }
        }
        val folders = listOf(
            Triple("home", "Home folder", homePath),
            Triple("downloads", "Downloads", "$homePath/Downloads"),
            Triple("documents", "Documents", "$homePath/Documents"),
            Triple("pictures", "Pictures", "$homePath/Pictures"),
        )
        folders.forEach { (id, label, path) ->
            File(homeDir, id.replaceFirstChar { it.uppercase() }).takeIf { id != "home" }?.mkdirs()
            File(appsDir, "droiddesk-folder-$id.desktop").writeText(
                """
                [Desktop Entry]
                Type=Application
                Name=$label
                Comment=Open $label in Linux Files
                Exec=thunar ${desktopEscape(path)}
                Icon=folder
                Terminal=false
                Categories=Utility;FileManager;
                Keywords=files;folder;search;$id;
                """.trimIndent() + "\n",
            )
        }
        listOf(
            Triple("wifi", "Wi-Fi settings", "network-wireless"),
            Triple("bluetooth", "Bluetooth settings", "bluetooth"),
            Triple("display", "Display and brightness", "video-display"),
            Triple("sound", "Sound and volume", "audio-volume-high"),
            Triple("hotspot", "Mobile hotspot", "network-transmit-receive"),
            Triple("battery", "Battery saver", "battery"),
        ).forEach { (id, label, icon) ->
            File(appsDir, "droiddesk-setting-$id.desktop").writeText(
                """
                [Desktop Entry]
                Type=Application
                Name=$label
                Comment=Open Android $label
                Exec=$pythonPath $launcherPath action:$id
                Icon=$icon
                Terminal=false
                Categories=Settings;System;
                Keywords=phone;android;control center;$id;
                """.trimIndent() + "\n",
            )
        }

    }

    private fun syncDockLaunchers(
        context: Context,
        homeDir: File,
        activities: List<android.content.pm.ResolveInfo>,
        appsDir: File,
    ) {
        val byPackage = activities.associateBy { it.activityInfo.packageName }
        val panelFile = File(homeDir, ".config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml")
        forgetDockAppsRemovedInPanel(context, panelFile)
        val dockPackages = getDockPackages(context)
        rememberSyncedDock(context, dockPackages)
        val dockEntries = dockPackages.mapIndexedNotNull { index, packageName ->
            if (!byPackage.containsKey(packageName)) return@mapIndexedNotNull null
            val safeName = packageName.replace(Regex("[^A-Za-z0-9_.-]"), "_")
            val source = File(appsDir, "$safeName.desktop")
            if (!source.isFile) return@mapIndexedNotNull null
            val id = 30 + index
            val dockDir = File(homeDir, ".config/xfce4/panel/launcher-$id").apply {
                mkdirs()
                listFiles()?.forEach { it.delete() }
            }
            val dockFile = File(dockDir, "droiddesk-android-$safeName.desktop")
            source.copyTo(dockFile, overwrite = true)
            id to dockFile.name
        }

        if (!panelFile.isFile) return
        val idStart = "<!-- DroidDesk Android dock ids start -->"
        val idEnd = "<!-- DroidDesk Android dock ids end -->"
        val pluginStart = "<!-- DroidDesk Android dock plugins start -->"
        val pluginEnd = "<!-- DroidDesk Android dock plugins end -->"
        var xml = withoutDockEntries(
            panelFile.readText()
                .replace(managedXmlBlock(idStart, idEnd), "")
                .replace(managedXmlBlock(pluginStart, pluginEnd), ""),
        )

        val idNeedle = Regex("<value type=\\\"int\\\" value=\\\"24\\\"/>")
        idNeedle.find(xml)?.let { match ->
            val indent = "        "
            val replacement = buildString {
                append('\n').append(indent).append(idStart).append('\n')
                dockEntries.forEach { (id, _) ->
                    append(indent).append("<value type=\"int\" value=\"$id\"/>").append('\n')
                }
                append(indent).append(idEnd).append('\n').append(match.value)
            }
            xml = xml.replaceRange(match.range, replacement)
        }

        val pluginNeedle = Regex("<property name=\\\"plugin-24\\\"")
        pluginNeedle.find(xml)?.let { match ->
            val indent = "    "
            val pluginBlock = buildString {
                append('\n').append(indent).append(pluginStart).append('\n')
                dockEntries.forEach { (id, filename) ->
                    append(
                        """
                        <property name="plugin-$id" type="string" value="launcher">
                          <property name="items" type="array">
                            <value type="string" value="$filename"/>
                          </property>
                        </property>
                        """.trimIndent().prependIndent(indent),
                    )
                    append('\n')
                }
                append(indent).append(pluginEnd).append('\n')
            }
            xml = xml.replaceRange(match.range, pluginBlock + match.value)
        }
        panelFile.writeText(xml)
        Log.i(TAG, "Added ${dockEntries.size} installed Android apps to the XFCE dock")
    }

    /**
     * The dock list lives in preferences and is rewritten into the panel on every
     * start, so an app removed in XFCE (right click > Remove) used to come back.
     * Compare the panel with what was synced last time and drop the missing ones.
     */
    private fun forgetDockAppsRemovedInPanel(context: Context, panelFile: File) {
        val preferences = context.getSharedPreferences("desktop_integration", Context.MODE_PRIVATE)
        val synced = preferences.getString("dock_synced", null) ?: return
        if (!panelFile.isFile) return
        val panelIds = panelPluginIds(panelFile.readText()) ?: return
        val removed = synced.split(',').mapNotNull { entry ->
            val (id, packageName) = entry.split(':', limit = 2).takeIf { it.size == 2 } ?: return@mapNotNull null
            packageName.takeIf { id.toIntOrNull() !in panelIds }
        }.toSet()
        if (removed.isEmpty()) return
        // All of them missing at once is not a choice made in XFCE (the panel dropped
        // every launcher at runtime on the tablet, 2026-10-05, and the dock came back
        // empty for good): keep the list, the sync below puts them back.
        val syncedCount = synced.split(',').count { it.contains(':') }
        if (removed.size == syncedCount && syncedCount > 1) {
            Log.w(TAG, "All Android dock apps missing from the panel; not treating it as a removal: $removed")
            return
        }
        setDockPackages(context, getDockPackages(context).filterNot { it in removed })
        Log.i(TAG, "Removed from the dock in XFCE, forgetting: $removed")
    }

    private fun rememberSyncedDock(context: Context, packages: List<String>) {
        context.getSharedPreferences("desktop_integration", Context.MODE_PRIVATE).edit()
            .putString("dock_synced", packages.mapIndexed { index, name -> "${30 + index}:$name" }.joinToString(","))
            .commit()
    }

    /** Plugin ids of the dock (panel-2), or null when the file cannot be read that way. */
    private fun panelPluginIds(xml: String): Set<Int>? {
        val panel = xml.indexOf("<property name=\"panel-2\"").takeIf { it >= 0 } ?: return null
        val ids = xml.indexOf("<property name=\"plugin-ids\"", panel).takeIf { it >= 0 } ?: return null
        val end = xml.indexOf("</property>", ids).takeIf { it >= 0 } ?: return null
        return Regex("value=\"(\\d+)\"").findAll(xml.substring(ids, end))
            .map { it.groupValues[1].toInt() }.toSet()
    }

    /** Updates the active xfconf session; editing its XML file alone is not enough while XFCE is running. */
    fun xfceDockCommand(context: Context): String {
        rememberSyncedDock(context, getDockPackages(context))
        val dock = getDockPackages(context).mapIndexed { index, packageName ->
            val safeName = packageName.replace(Regex("[^A-Za-z0-9_.-]"), "_")
            (30 + index) to "droiddesk-android-$safeName.desktop"
        }
        // Plugin 26 is the battery (genmon), added by droiddesk-xfce-tweaks.
        val battery = if (File(context.filesDir, "usr/lib/xfce4/panel/plugins/libgenmon.so").exists()) {
            listOf(26)
        } else {
            emptyList()
        }
        // Plugin 27 is the systray for the "USB" icon, added once pygobject is installed.
        val tray = if (File(context.filesDir, "usr/lib").listFiles().orEmpty()
                .any { File(it, "site-packages/gi/__init__.py").isFile }
        ) {
            listOf(27)
        } else {
            emptyList()
        }
        val pluginIds = listOf(20, 21, 22, 23) + dock.map { it.first } + listOf(24, 2) + tray + battery + listOf(3, 25)
        return buildString {
            for (id in 30..37) {
                append("xfconf-query -c xfce4-panel -p /plugins/plugin-$id -r >/dev/null 2>&1 || true; ")
            }
            append("xfconf-query -c xfce4-panel -p /panels/panel-2/plugin-ids -a ")
            pluginIds.forEach { id -> append("-t int -s $id ") }
            append(">/dev/null 2>&1; ")
            dock.forEach { (id, filename) ->
                append("xfconf-query -c xfce4-panel -p /plugins/plugin-$id -n -t string -s launcher >/dev/null 2>&1; ")
                append("xfconf-query -c xfce4-panel -p /plugins/plugin-$id/items -n -a -t string -s '$filename' >/dev/null 2>&1; ")
            }
        }
    }

    /**
     * xfconfd rewrites xfce4-panel.xml without comments, so the dock ids lost their
     * DroidDesk markers, survived the marker cleanup and were inserted again on every
     * start: 30, 31, 32 five times after five starts, drawn as empty grey launchers
     * (tablet). Removes the dock's ids (30–37) from panel-2's plugin-ids, with or without
     * markers, drops duplicate ids, and removes unmarked plugin-30…37 launcher definitions;
     * syncLaunchers then inserts them once.
     */
    private fun withoutDockEntries(input: String): String {
        var xml = input.replace(
            Regex(
                "\\s*<property name=\"plugin-3[0-7]\" type=\"string\" value=\"launcher\"" +
                    "(?:/>|>\\s*(?:<property name=\"items\" type=\"array\">.*?</property>\\s*)?</property>)",
                RegexOption.DOT_MATCHES_ALL,
            ),
            "",
        )
        val panel = xml.indexOf("<property name=\"panel-2\"").takeIf { it >= 0 } ?: return xml
        val idsStart = xml.indexOf("<property name=\"plugin-ids\"", panel).takeIf { it >= 0 } ?: return xml
        val idsEnd = xml.indexOf("</property>", idsStart).takeIf { it >= 0 } ?: return xml
        val seen = mutableSetOf<Int>()
        val section = Regex("\\s*<value type=\"int\" value=\"(\\d+)\"/>").replace(xml.substring(idsStart, idsEnd)) { match ->
            val id = match.groupValues[1].toInt()
            if (id in 30..37 || !seen.add(id)) "" else match.value
        }
        xml = xml.substring(0, idsStart) + section + xml.substring(idsEnd)
        return xml
    }

    private fun managedXmlBlock(start: String, end: String): Regex = Regex(
        "${Regex.escape(start)}.*?${Regex.escape(end)}",
        RegexOption.DOT_MATCHES_ALL,
    )

    private fun serve(context: Context, socket: LocalServerSocket) {
        while (server === socket) {
            var client: LocalSocket? = null
            try {
                client = socket.accept()
                if (client.peerCredentials.uid != android.os.Process.myUid()) {
                    Log.w(TAG, "Rejected launcher request from uid ${client.peerCredentials.uid}")
                    continue
                }
                val command = client.inputStream.bufferedReader().readLine()?.trim().orEmpty()
                if (command.startsWith("action:")) {
                    launchSystemAction(context, command.removePrefix("action:"))
                } else if (command.startsWith("open:")) {
                    openInAndroid(context, command.removePrefix("open:"))
                } else if (command.startsWith("xdg:")) {
                    // Linux app first; Android only when nothing on the desktop takes it.
                    val target = command.removePrefix("xdg:")
                    if (!LinuxRuntime(context).openWithLinuxApp(target)) openInAndroid(context, target)
                } else {
                    launchPackage(context, command)
                }
            } catch (error: Exception) {
                if (server === socket) Log.w(TAG, "Android app launcher request failed", error)
            } finally {
                runCatching { client?.close() }
            }
        }
    }

    private fun launchPackage(context: Context, packageName: String): Boolean {
        if (!packageName.matches(Regex("[A-Za-z0-9_.]+"))) return false
        val intent = context.packageManager.getLaunchIntentForPackage(packageName) ?: return false
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return runCatching { context.startActivity(intent); true }
            .onFailure { Log.w(TAG, "Could not launch Android package $packageName", it) }
            .getOrDefault(false)
    }

    /**
     * droiddesk-open: a URL opens in its Android handler, a file (absolute host
     * path) goes out as a content:// URI. termux-open cannot do this here: it
     * broadcasts to the Termux app, which is not installed.
     */
    private fun openInAndroid(context: Context, target: String) {
        val intent = if (Regex("^[A-Za-z][A-Za-z0-9+.-]*:").containsMatchIn(target) &&
            !target.startsWith("file:")
        ) {
            Intent(Intent.ACTION_VIEW, Uri.parse(target))
        } else {
            // Only an explicit file: URI is parsed; a bare path is taken as is, so
            // names like "report#1.pdf" or "50%.png" keep their # ? and %.
            val path = if (target.startsWith("file:")) Uri.parse(target).path else target
            val file = File(path ?: return).canonicalFile
            if (!file.isFile) {
                Log.w(TAG, "droiddesk-open: no such file $file")
                return
            }
            val uri = runCatching {
                FileProvider.getUriForFile(context, "${context.packageName}.files", file)
            }.getOrElse {
                Log.w(TAG, "droiddesk-open: $file is outside the shareable paths", it)
                return
            }
            val type = MimeTypeMap.getSingleton()
                .getMimeTypeFromExtension(file.extension.lowercase()) ?: "*/*"
            Intent(Intent.ACTION_VIEW).setDataAndType(uri, type)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(intent, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(chooser) }
            .onFailure { Log.w(TAG, "droiddesk-open: nothing can open $target", it) }
    }

    fun launchSystemAction(context: Context, action: String) {
        val settingsAction = when (action) {
            "wifi" -> Settings.ACTION_WIFI_SETTINGS
            "bluetooth" -> Settings.ACTION_BLUETOOTH_SETTINGS
            "display" -> Settings.ACTION_DISPLAY_SETTINGS
            "sound" -> Settings.ACTION_SOUND_SETTINGS
            "hotspot" -> "android.settings.TETHER_SETTINGS"
            "battery" -> Settings.ACTION_BATTERY_SAVER_SETTINGS
            else -> Settings.ACTION_SETTINGS
        }
        runCatching {
            context.startActivity(Intent(settingsAction).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }.onFailure { Log.w(TAG, "Could not open Android setting $action", it) }
    }

    private fun launcherActivities(context: Context): List<android.content.pm.ResolveInfo> {
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        return context.packageManager.queryIntentActivities(intent, PackageManager.MATCH_ALL)
            .filter { it.activityInfo.packageName != context.packageName }
            .distinctBy { it.activityInfo.packageName }
            .sortedBy { it.loadLabel(context.packageManager).toString().lowercase() }
    }

    private fun defaultDockPackages(
        context: Context,
        activities: List<android.content.pm.ResolveInfo>,
    ): List<String> {
        val installed = activities.map { it.activityInfo.packageName }.toSet()
        val packageManager = context.packageManager
        val defaultDialer = packageManager.resolveActivity(
            Intent(Intent.ACTION_DIAL, Uri.parse("tel:")), PackageManager.MATCH_DEFAULT_ONLY,
        )?.activityInfo?.packageName
        val defaultMessages = packageManager.resolveActivity(
            Intent(Intent.ACTION_SENDTO, Uri.parse("smsto:")), PackageManager.MATCH_DEFAULT_ONLY,
        )?.activityInfo?.packageName
        return listOf(
            listOf(defaultDialer, "com.google.android.dialer", "com.android.dialer", "com.samsung.android.dialer"),
            listOf(defaultMessages, "com.google.android.apps.messaging", "com.android.messaging", "com.samsung.android.messaging"),
            listOf("com.whatsapp", "com.whatsapp.w4b"),
            listOf("com.android.chrome"),
        ).mapNotNull { choices ->
            choices.filterNotNull().firstOrNull { it in installed }
        }.distinct()
    }

    private fun desktopEscape(value: String): String = value
        .replace("\\", "\\\\")
        .replace("\n", " ")

    private fun writeIconPng(drawable: android.graphics.drawable.Drawable, output: File): Boolean {
        return runCatching {
            val size = 192
            val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            drawable.setBounds(0, 0, size, size)
            drawable.draw(canvas)
            output.outputStream().use { stream ->
                check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream))
            }
            bitmap.recycle()
            true
        }.getOrElse { error ->
            Log.w(TAG, "Could not export Android app icon to ${output.name}", error)
            false
        }
    }
}
