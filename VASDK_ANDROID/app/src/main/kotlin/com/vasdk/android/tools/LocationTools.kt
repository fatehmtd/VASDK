package com.vasdk.android.tools

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.Uri
import android.os.Bundle
import androidx.core.content.ContextCompat
import org.json.JSONObject

object LocationTools {

    val toolNames = setOf("get_location", "navigate_to")

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "get_location" -> getLocation(context)
        "navigate_to"  -> navigateTo(context, argsJson)
        else           -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    private fun hasPermission(context: Context, permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    //  get_location 

    private fun getLocation(context: Context): String {
        if (!hasPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) &&
            !hasPermission(context, Manifest.permission.ACCESS_COARSE_LOCATION)) {
            return JSONObject().put(
                "error",
                "Cannot access location: the user has not granted Location access. " +
                "Tell the user they can enable the Location permission for this app in Android settings."
            ).toString()
        }

        val lm = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager

        // Try last known location first (instant, no callback needed)
        val lastKnown = listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)
            .mapNotNull { runCatching { lm.getLastKnownLocation(it) }.getOrNull() }
            .maxByOrNull { it.time }

        if (lastKnown != null && System.currentTimeMillis() - lastKnown.time < 60_000) {
            return locationToJson(lastKnown)
        }

        // Request a fresh fix with 10 s timeout
        val result = AsyncBridge.blockingCall<Location>(10_000) { future ->
            val provider = when {
                lm.isProviderEnabled(LocationManager.GPS_PROVIDER)     -> LocationManager.GPS_PROVIDER
                lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER) -> LocationManager.NETWORK_PROVIDER
                else -> null
            }
            if (provider == null) {
                future.complete(null)
                return@blockingCall
            }
            val listener = object : LocationListener {
                override fun onLocationChanged(loc: Location) {
                    lm.removeUpdates(this)
                    future.complete(loc)
                }
                @Deprecated("Deprecated in API 29")
                override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
            }
            try {
                lm.requestSingleUpdate(provider, listener, null)
            } catch (e: Exception) {
                future.complete(null)
            }
        }
        return if (result != null) locationToJson(result)
        else JSONObject().put("error", "Location fix timed out after 10 s").toString()
    }

    private fun locationToJson(loc: Location) = JSONObject().apply {
        put("latitude",   loc.latitude)
        put("longitude",  loc.longitude)
        put("accuracy_m", loc.accuracy.toDouble())
        put("provider",   loc.provider ?: "unknown")
    }.toString()

    //  navigate_to 

    private fun navigateTo(context: Context, argsJson: String): String {
        val address = JSONObject(argsJson).getString("address")
        val uri     = Uri.parse("geo:0,0?q=${Uri.encode(address)}")
        return IntentDispatch.fire(context, Intent(Intent.ACTION_VIEW, uri), "navigation to $address")
    }
}
