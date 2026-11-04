package com.vasdk.android.tools

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject

/**
 * The phone book as the agent's @contact entity, and the place_call tool its "call mom" uses
 * (assets/agents/phone/agent.txtpb).
 */
object ContactsEntity {

    /**
     * Every contact with a number, for VasdkEngine.setEntityValues("contact", ...): the number as
     * the value, the name as spoken, and the name and first name as what the user may say.
     * An empty array without the contacts permission.
     */
    fun values(context: Context): String {
        val values = JSONArray()
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED)
            return values.toString()
        val seen = mutableSetOf<String>()
        val columns = arrayOf(Phone.DISPLAY_NAME, Phone.NUMBER, Phone.IS_SUPER_PRIMARY)
        context.contentResolver.query(Phone.CONTENT_URI, columns, null, null, "${Phone.IS_SUPER_PRIMARY} DESC")?.use { cursor ->
            while (cursor.moveToNext()) {
                val name = cursor.getString(0)?.trim().orEmpty()
                val number = cursor.getString(1)?.filter { it.isDigit() || it == '+' }.orEmpty()
                if (name.isEmpty() || number.isEmpty() || !seen.add(name.lowercase())) continue   // one number per name
                val synonyms = JSONArray().put(name.lowercase())
                val first = name.substringBefore(' ').lowercase()
                if (first != name.lowercase()) synonyms.put(first)
                values.put(JSONObject().put("value", number).put("spoken", name).put("synonyms", synonyms))
            }
        }
        return values.toString()
    }

    /** place_call {"number"}: calls it; the agent has already confirmed with the user. */
    fun placeCall(context: Context, argsJson: String): String {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CALL_PHONE) != PackageManager.PERMISSION_GRANTED)
            return JSONObject().put("error", "the Phone permission isn't granted").toString()
        val number = JSONObject(argsJson).optString("number").filter { it.isDigit() || it == '+' }
        if (number.isEmpty()) return JSONObject().put("error", "no number").toString()
        return IntentDispatch.fire(context, Intent(Intent.ACTION_CALL, Uri.parse("tel:$number")), "calling $number")
    }
}
