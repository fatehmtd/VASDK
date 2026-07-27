package com.vasdk.android.tools

import android.content.Context
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.StatFs
import android.provider.Settings
import org.json.JSONArray
import org.json.JSONObject

object SystemTools {

    val toolNames = setOf(
        "get_battery_status",
        "get_device_info",
        "get_wifi_info",
        "get_network_status",
        "get_storage_info",
        "get_screen_brightness",
        "set_screen_brightness",
        "set_ringer_mode",
        "get_installed_apps"
    )

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "get_battery_status"    -> getBatteryStatus(context)
        "get_device_info"       -> getDeviceInfo()
        "get_wifi_info"         -> getWifiInfo(context)
        "get_network_status"    -> getNetworkStatus(context)
        "get_storage_info"      -> getStorageInfo()
        "get_screen_brightness" -> getScreenBrightness(context)
        "set_screen_brightness" -> setScreenBrightness(context, argsJson)
        "set_ringer_mode"       -> setRingerMode(context, argsJson)
        "get_installed_apps"    -> getInstalledApps(context)
        else                    -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    //  Battery 

    private fun getBatteryStatus(context: Context): String {
        val bm = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
        return JSONObject().apply {
            put("level_percent", bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY))
            put("is_charging", bm.isCharging)
        }.toString()
    }

    //  Device info 

    private fun getDeviceInfo(): String = JSONObject().apply {
        put("manufacturer", Build.MANUFACTURER)
        put("model", Build.MODEL)
        put("android_version", Build.VERSION.RELEASE)
        put("sdk_int", Build.VERSION.SDK_INT)
    }.toString()

    //  WiFi 

    private fun getWifiInfo(context: Context): String {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val wm = context.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        val isWifi = caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true

        return JSONObject().apply {
            put("is_wifi_connected", isWifi)
            if (isWifi) {
                val info = wm.connectionInfo
                put("rssi_dbm", info.rssi)
                put("signal_level", WifiManager.calculateSignalLevel(info.rssi, 5))
                // SSID requires ACCESS_FINE_LOCATION in Android 9+; return if available
                val ssid = info.ssid?.removeSurrounding("\"") ?: ""
                if (ssid.isNotEmpty() && ssid != "<unknown ssid>") put("ssid", ssid)
                else put("ssid_note", "Location permission required to read SSID")
            }
        }.toString()
    }

    //  Network status 

    private fun getNetworkStatus(context: Context): String {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        val connected = caps != null
        val type = when {
            caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)     == true -> "wifi"
            caps?.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) == true -> "cellular"
            caps?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true -> "ethernet"
            else -> "none"
        }
        return JSONObject().apply {
            put("connected", connected)
            put("type", type)
            if (connected) {
                put("has_internet", caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true)
            }
        }.toString()
    }

    //  Storage 

    private fun getStorageInfo(): String {
        val stat = StatFs(android.os.Environment.getDataDirectory().path)
        val blockSize = stat.blockSizeLong
        val totalBytes = stat.blockCountLong * blockSize
        val freeBytes = stat.availableBlocksLong * blockSize
        val gb = 1_073_741_824.0
        return JSONObject().apply {
            put("total_gb", String.format("%.2f", totalBytes / gb).toDouble())
            put("free_gb", String.format("%.2f", freeBytes / gb).toDouble())
            put("used_gb", String.format("%.2f", (totalBytes - freeBytes) / gb).toDouble())
        }.toString()
    }

    //  Screen brightness 

    private fun getScreenBrightness(context: Context): String {
        val level = Settings.System.getInt(
            context.contentResolver, Settings.System.SCREEN_BRIGHTNESS, -1
        )
        return JSONObject().apply {
            put("brightness", level)
            put("max", 255)
        }.toString()
    }

    private fun setScreenBrightness(context: Context, argsJson: String): String {
        if (!Settings.System.canWrite(context)) {
            return JSONObject().put(
                "error",
                "Modify system settings permission not granted. Ask the user to enable it in Settings → Apps → Special app access → Modify system settings"
            ).toString()
        }
        val args = JSONObject(argsJson)
        val level = args.getInt("level").coerceIn(0, 255)
        Settings.System.putInt(context.contentResolver, Settings.System.SCREEN_BRIGHTNESS, level)
        return JSONObject().apply {
            put("brightness_set", level)
            put("max", 255)
        }.toString()
    }

    //  Ringer mode 

    private fun setRingerMode(context: Context, argsJson: String): String {
        val args = JSONObject(argsJson)
        val mode = args.getString("mode").lowercase()
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val ringerMode = when (mode) {
            "normal"   -> AudioManager.RINGER_MODE_NORMAL
            "vibrate"  -> AudioManager.RINGER_MODE_VIBRATE
            "silent"   -> AudioManager.RINGER_MODE_SILENT
            else       -> return JSONObject().put("error", "Invalid mode '$mode'. Use: normal, vibrate, or silent").toString()
        }
        return try {
            am.ringerMode = ringerMode
            JSONObject().put("ringer_mode", mode).toString()
        } catch (e: SecurityException) {
            JSONObject().put("error", "Do Not Disturb policy prevents changing ringer mode: ${e.message}").toString()
        }
    }

    //  Installed apps 

    private fun getInstalledApps(context: Context): String {
        val pm = context.packageManager
        val apps = pm.getInstalledApplications(0)
            .filter { it.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM == 0 }
            .mapNotNull { pm.getApplicationLabel(it)?.toString() }
            .sorted()
        val arr = JSONArray()
        apps.forEach { arr.put(it) }
        return JSONObject().put("apps", arr).toString()
    }
}
