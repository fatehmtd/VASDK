// The model store from Kotlin (docs/models.md): downloads the model files local providers load,
// checks each against the SHA-256 its manifest pins, and keeps them where a session looks when
// its models_dir is unset.
package com.vasdk

import com.sun.jna.Pointer
import com.sun.jna.ptr.PointerByReference
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonPrimitive
import java.io.Closeable

/**
 * ```
 * Models().use { models ->                              // VASDK_MODELS_DIR, else the user cache
 *     for (id in models.forConfig("engine.txtpb")) models.pull(id) { model, _, done, total -> println("$model $done/$total"); true }
 * }
 * ```
 * On Android give `directory` (the app's files directory): there is no user cache to default to.
 * `manifestPath`: another manifest than the built-in one; `token`: a Hugging Face token for gated
 * models (else HF_TOKEN); `offline`: never download.
 */
class Models(
    directory: String? = null,
    manifestPath: String? = null,
    token: String? = null,
    offline: Boolean = false,
    architecture: String? = null,
) : Closeable {
    /** One model of the manifest, for this machine. */
    data class Model(
        val id: String,
        val description: String,
        val providers: List<String>,
        val license: String,
        val licenseUrl: String,
        val revision: String,
        val gated: Boolean,
        val requiresAcceptance: Boolean,
        val size: Long,
        val status: String,   // "present", "partial", "missing"
    )

    /** Bytes of `model` in the store so far, of `total`; `file` downloading. Return false to stop. */
    fun interface Progress {
        fun report(model: String, file: String, done: Long, total: Long): Boolean
    }

    private var handle: Pointer? = run {
        val options = buildMap<String, JsonPrimitive> {
            directory?.let { put("directory", JsonPrimitive(it)) }
            manifestPath?.let { put("manifest_path", JsonPrimitive(it)) }
            token?.let { put("token", JsonPrimitive(it)) }
            if (offline) put("offline", JsonPrimitive(true))
            architecture?.let { put("architecture", JsonPrimitive(it)) }
        }
        val error = PointerByReference()
        Lib.vasdk.vasdk_models_create(if (options.isEmpty()) null else JsonObject(options).toString(), error)
            ?: throw VasdkException(taken(error.value))
    }
    private val live get() = handle ?: throw VasdkException("model store closed")
    @Volatile private var pulling: ModelProgressFn? = null

    val directory: String get() = Lib.vasdk.vasdk_models_directory(live)

    /** Every model of this machine, with its status. */
    fun list(): List<Model> = Json.parseToJsonElement(taken(Lib.vasdk.vasdk_models_list(live))).jsonArray.map { element ->
        val model = element as JsonObject
        fun text(key: String) = model[key]?.jsonPrimitive?.content ?: ""
        Model(
            id = text("id"),
            description = text("description"),
            providers = (model["providers"] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList(),
            license = text("license"),
            licenseUrl = text("license_url"),
            revision = text("revision"),
            gated = text("gated") == "true",
            requiresAcceptance = text("requires_acceptance") == "true",
            size = text("size").toLongOrNull() ?: 0,
            status = text("status"),
        )
    }

    /** The ids of the models an engine config's providers load. */
    fun forConfig(configPath: String): List<String> {
        val error = PointerByReference()
        val ids = Lib.vasdk.vasdk_models_for_config(live, configPath, error) ?: throw VasdkException(taken(error.value))
        return Json.parseToJsonElement(taken(ids)).jsonArray.map { it.jsonPrimitive.content }
    }

    /** "present", "partial", "missing", or "unknown" (not in the manifest). */
    fun status(id: String): String = when (Lib.vasdk.vasdk_models_status(live, id)) {
        2 -> "present"
        1 -> "partial"
        0 -> "missing"
        else -> "unknown"
    }

    /**
     * Downloads what is missing of the model, resuming a stopped download; blocks until done, so
     * call it off the main thread. `acceptLicense`: you read the license of a model that
     * requiresAcceptance. `progress` returning false stops the pull.
     */
    fun pull(id: String, acceptLicense: Boolean = false, progress: Progress? = null) {
        val callback = progress?.let {
            ModelProgressFn { model, file, done, total, _ ->
                try {
                    if (it.report(model.utf8(), file.utf8(), done, total)) 1 else 0
                } catch (_: Throwable) {
                    0   // an exception can't cross into C: stop the pull
                }
            }
        }
        val error = PointerByReference()
        pulling = callback   // reachable while C may call it
        val pulled = try {
            Lib.vasdk.vasdk_models_pull(live, id, if (acceptLicense) 1 else 0, callback, null, error)
        } finally {
            pulling = null
        }
        if (pulled == 0) throw VasdkException(taken(error.value))
    }

    /** Hashes the model's files; throws when one is missing or doesn't match. */
    fun verify(id: String) {
        val error = PointerByReference()
        if (Lib.vasdk.vasdk_models_verify(live, id, error) == 0) throw VasdkException(taken(error.value))
    }

    fun remove(id: String) {
        val error = PointerByReference()
        if (Lib.vasdk.vasdk_models_remove(live, id, error) == 0) throw VasdkException(taken(error.value))
    }

    override fun close() {
        handle?.let { Lib.vasdk.vasdk_models_free(it) }
        handle = null
    }

    companion object {
        /** Where sessions look when models_dir is unset, and where a Models() downloads. */
        fun defaultDirectory(): String = taken(Lib.vasdk.vasdk_models_default_directory())
    }
}
