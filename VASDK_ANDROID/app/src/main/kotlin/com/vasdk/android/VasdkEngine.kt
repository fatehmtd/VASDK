package com.vasdk.android

import android.content.Context
import android.content.ContextWrapper
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.annotation.Keep
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/** Download URL, GGUF filename, and chat format for one local LLM preset. */
data class LLMModelMeta(
    val url: String,
    val modelName: String,
    val format: String
)

/** On-device vs cloud STT provider choice, exposed via the Settings UI. */
enum class SttProvider(val id: String, val label: String) {
    PARAKEET("parakeet", "Parakeet (on-device)"),
    SONIOX("soniox", "Soniox (cloud)"),
    DEEPGRAM("deepgram", "Deepgram (cloud)"),
    GLADIA("gladia", "Gladia (cloud)"),
    CARTESIA("cartesia", "Cartesia (cloud)"),
    FISHAUDIO("fishaudio", "Fish Audio (cloud)"),
    GRADIUM("gradium", "Gradium (cloud)"),
    WHISTLE("whistle", "Whistle (on-device)")
}

/** On-device vs cloud TTS provider choice, exposed via the Settings UI. */
enum class TtsProvider(val id: String, val label: String) {
    POCKET("pocket", "Pocket TTS (on-device)"),
    SONIOX("soniox", "Soniox (cloud)"),
    DEEPGRAM("deepgram", "Deepgram (cloud)"),
    CARTESIA("cartesia", "Cartesia (cloud)"),
    FISHAUDIO("fishaudio", "Fish Audio (cloud)"),
    GRADIUM("gradium", "Gradium (cloud)"),
    PHONON("phonon", "Phonon (on-device)"),
    MOSS("moss", "MOSS-TTS-Nano (on-device, ~710 MB)")
}

/**
 * One parameter of a [ToolInfo], as declared by the native Tool schema.
 */
data class ToolParamInfo(
    val name: String,
    val type: String,
    val required: Boolean,
    val description: String
)

/**
 * One tool currently registered with the LLM (send_sms, get_contacts, etc.), structured
 * for display rather than as the raw JSON schema text fed to the model.
 */
data class ToolInfo(
    val name: String,
    val description: String,
    val parameters: List<ToolParamInfo>
)

/**
 * Snapshot of the currently active LLM for the "model info" screen: which model is
 * loaded, its chat format, the base system prompt, the tools it currently has access to,
 * and a preview of the actual prompt/request the transport sends (cloud: the JSON request
 * payload; on-device: the rendered chat-format prompt) for an advanced/raw view.
 */
data class ModelInfo(
    val isLocal: Boolean,
    val modelName: String,
    val chatFormat: String,
    val systemPrompt: String,
    val tools: List<ToolInfo>,
    val rawPrompt: String
)

/**
 * Thin Kotlin wrapper around the native VASDK pipeline.
 *
 * Lifecycle:
 *   1. init()      – create the audio streams and pipeline graph
 *   2. start()     – start capturing microphone audio
 *   3. stop()      – pause the pipeline (audio streams remain open)
 *   4. release()   – tear down everything; call init() again to reuse
 */
class VasdkEngine private constructor(
    private val contextWrapper: ContextWrapper
) {
    var useLocalLLM: Boolean = LlmPreferences.isLocalLlmEnabled(contextWrapper)
        private set
    var llmModelMeta: LLMModelMeta = LOCAL_LLM_MODELS.firstOrNull {
        it.modelName == ProviderPreferences.getLocalLlmModelName(contextWrapper)
    } ?: LFM2_5Meta
        private set
    var sttProvider: SttProvider = ProviderPreferences.getSttProvider(contextWrapper)
        private set
    var sttStreaming: Boolean = ProviderPreferences.getSttStreaming(contextWrapper)
        private set
    var ttsProvider: TtsProvider = ProviderPreferences.getTtsProvider(contextWrapper)
        private set

    /**
     * Switches between local (on-device) and remote LLM mode and persists the choice.
     * Caller is responsible for stopping/releasing the pipeline beforehand and
     * re-running init()/downloadAndInit() afterwards to pick up the new mode.
     */
    fun setLlmMode(useLocal: Boolean) {
        useLocalLLM = useLocal
        LlmPreferences.setLocalLlmEnabled(contextWrapper, useLocal)
    }

    /** Which GGUF the local LLM loads when [useLocalLLM] is true. Persists the choice. */
    fun setLocalLlmModel(meta: LLMModelMeta) {
        llmModelMeta = meta
        ProviderPreferences.setLocalLlmModelName(contextWrapper, meta.modelName)
    }

    /**
     * Switches the STT/TTS provider and persists the choice. Caller is responsible for
     * stopping/releasing the pipeline beforehand and re-running init()/downloadAndInit()
     * afterwards to pick up the new provider.
     */
    fun setSttProvider(provider: SttProvider) {
        sttProvider = provider
        ProviderPreferences.setSttProvider(contextWrapper, provider)
    }

    /** Whether the STT provider should process continuous audio (true) or discrete segments (false). Persists the choice. */
    fun setSttStreaming(streaming: Boolean) {
        sttStreaming = streaming
        ProviderPreferences.setSttStreaming(contextWrapper, streaming)
    }

    fun setTtsProvider(provider: TtsProvider) {
        ttsProvider = provider
        ProviderPreferences.setTtsProvider(contextWrapper, provider)
    }

    interface Listener {
        fun onTranscriptReceived(text: String, isFinal: Boolean)
        fun onStatusChanged(status: String)
        fun onLlmResponse(chunk: String, isFinal: Boolean, isExpecting: Boolean, choicesJson: String) {}
        fun onDownloadProgress(fileProgress: Float, overallProgress: Float, fileName: String) {}
        // features is a flat FloatArray packed as [rms, bass, lowMid, mid, hiMid, treble, ...]
        // with one group of 6 per analysis window (stride = 6).
        fun onAudioFeatures(source: String, features: FloatArray) {}
        /**
         * Called when an error occurs in the pipeline.
         * @param errorCode Same codes as defined in HttpTransport/NetworkError
         */
        fun onError(errorCode: Int, errorMessage: String)

        /**
         * Called synchronously from a native worker thread when the LLM requests a tool.
         * Implement this to handle Android-specific tools (battery, volume, etc.).
         * Must return a JSON string with the tool result.
         * Do NOT post work to the main thread - this call must return immediately.
         */
        fun onToolCall(toolName: String, argsJson: String): String = "{}"
    }

    private var listener: Listener? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    fun setListener(l: Listener?) {
        listener = l
    }

    fun isReady(): Boolean = isNativeInitialized

    fun isStarted(): Boolean = isNativeStarted

    fun downloadAndInit(
        onProgress: ((Float, Float, String) -> Unit)? = null,
        onInitComplete: (Boolean) -> Unit
    ) {
        val modelRequests = mutableListOf<ModelDownloadRequest>()
        if (sttProvider == SttProvider.PARAKEET) {
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(contextWrapper.cacheDir, "parakeet"),
                    baseUrl = "https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx/resolve/main",
                    files = listOf(
                        "encoder-model.int8.onnx",
                        "decoder_joint-model.int8.onnx",
                        "nemo128.onnx",
                        "vocab.txt"
                    )
                )
            )
        }
        if (sttProvider == SttProvider.WHISTLE) {
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(contextWrapper.cacheDir, "whistle"),
                    baseUrl = "https://huggingface.co/Cactus-Compute/whistle/resolve/741118687e1f008f06d1351a402a731274789790",
                    files = listOf("whistle.cact")
                )
            )
        }
        if (ttsProvider == TtsProvider.PHONON) {
            // phonon-31m-en: the public (non-gated) English checkpoint, CC BY-NC 4.0.
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(contextWrapper.cacheDir, "phonon_tts"),
                    baseUrl = "https://huggingface.co/gradium/phonon-31m-en/resolve/92741c5b07d085ff840e26fea8ce43110b87852f",
                    files = listOf(
                        "config.json",
                        "tokenizer.model",
                        "model.q8.gguf",
                        "default-voice.safetensors"
                    )
                )
            )
        }
        if (ttsProvider == TtsProvider.MOSS) {
            // MOSS-TTS-Nano 100M (Apache 2.0) and the streaming decoder of its audio tokenizer,
            // side by side as the manifest expects (it points at ../MOSS-Audio-Tokenizer-Nano-ONNX).
            val mossDir = File(contextWrapper.cacheDir, "moss_tts")
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(mossDir, "MOSS-TTS-Nano-100M-ONNX"),
                    baseUrl = "https://huggingface.co/Supbatomic/moss-tts-nano-100m-onnx-model/resolve/ec4afe55ae63d93fa15a1aed3730ccfd38e6bb86",
                    files = listOf(
                        "browser_poc_manifest.json",
                        "tts_browser_onnx_meta.json",
                        "tokenizer.model",
                        "moss_tts_prefill.onnx",
                        "moss_tts_decode_step.onnx",
                        "moss_tts_global_shared.data",
                        "moss_tts_local_fixed_sampled_frame.onnx",
                        "moss_tts_local_shared.data"
                    )
                )
            )
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(mossDir, "MOSS-Audio-Tokenizer-Nano-ONNX"),
                    baseUrl = "https://huggingface.co/OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX/resolve/ceff0d0749bfb3fa2d61149794ec6feef0d1e1ae",
                    files = listOf(
                        "codec_browser_onnx_meta.json",
                        "moss_audio_tokenizer_decode_step.onnx",
                        "moss_audio_tokenizer_decode_shared.data"
                    )
                )
            )
        }
        // pyannote 3.1 speaker embedding (WeSpeaker ResNet34-LM), always on like the desktop app.
        modelRequests.add(
            ModelDownloadRequest(
                targetDir = File(contextWrapper.cacheDir, "pyannote"),
                baseUrl = "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models",
                files = listOf(SPEAKER_MODEL_FILE)
            )
        )
        if (ttsProvider == TtsProvider.POCKET) {
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(contextWrapper.cacheDir, "pocket_tts"),
                    baseUrl = "https://huggingface.co/spaces/KevinAHM/pocket-tts-web/resolve/main/onnx/english_2026-04",
                    files = listOf(
                        "text_conditioner_int8.onnx",
                        "flow_lm_main_int8.onnx",
                        "flow_lm_flow_int8.onnx",
                        "mimi_decoder_int8.onnx",
                        "tokenizer.model",
                        "voices.bin"
                    )
                )
            )
        }
        if(useLocalLLM) {
            modelRequests.add(
                ModelDownloadRequest(
                    targetDir = File(contextWrapper.cacheDir, "llm"),
                    baseUrl = llmModelMeta.url,
                    files = listOf(
                        llmModelMeta.modelName
                    )
                )
            )
        }
        val totalFiles = modelRequests.sumOf { it.files.size }
        Log.i(TAG, "downloadAndInit: Preparing to download $totalFiles files.")
        downloadModels(requests = modelRequests, onProgress = onProgress) { success, paths ->
            if (success) {
                Log.i(TAG, "Successfully downloaded models: $paths")
                Thread {
                    val ok = init()
                    mainHandler.post { onInitComplete.invoke(ok) }
                }.start()
            } else {
                Log.e(TAG, "Failed to download models")
                onInitComplete.invoke(false)
            }
        }
    }

    fun init(): Boolean {
        synchronized(VasdkEngine::class.java) {
            if (isNativeInitialized) return true

            val modelMap = mutableMapOf(
                R.raw.silero_vad to "",
                R.raw.smart_turn_v3 to ""
            )
            val modelFiles = arrayOf(R.raw.silero_vad, R.raw.smart_turn_v3)

            for (modelFile in modelFiles) {
                val modelPath = extractEmbeddedModels(modelFile, contextWrapper.cacheDir)
                if (modelPath == null) {
                    Log.e(TAG, "Failed to extract model file $modelFile")
                    return false
                } else {
                    modelMap[modelFile] = modelPath
                }
            }

            val llmModelFile = File(contextWrapper.cacheDir, "llm/${llmModelMeta.modelName}")

            if (!llmModelFile.exists() and useLocalLLM) {
                Log.e(TAG, "LLM model not found at ${llmModelFile.absolutePath}")
                return false
            }

            // Model dirs are only meaningful for the currently selected on-device provider;
            // the native side ignores them when the cloud (Soniox) provider is active.
            val sttModelDir = File(contextWrapper.cacheDir,
                if (sttProvider == SttProvider.WHISTLE) "whistle" else "parakeet")
            val ttsModelDir = File(contextWrapper.cacheDir,
                when (ttsProvider) {
                    TtsProvider.PHONON -> "phonon_tts"
                    TtsProvider.MOSS -> "moss_tts"
                    else -> "pocket_tts"
                })
            val speakerModelFile = File(contextWrapper.cacheDir, "pyannote/$SPEAKER_MODEL_FILE")

            // sonioxpp's TLS backend on Android (mbedTLS) has no visibility into the
            // system trust store, so WSS/HTTPS handshakes fail cert verification unless
            // we hand it a CA bundle explicitly. Re-extracted on every init() so it tracks
            // whatever the device currently trusts (OS updates, MDM-installed CAs, etc).
            val caBundlePath = extractSystemCaBundle(contextWrapper.filesDir) ?: ""
            if (caBundlePath.isEmpty()) {
                Log.e(TAG, "Failed to extract system CA bundle; cloud TLS connections will fail")
            }

            isNativeInitialized = nativeInit(
                vadModelPath = modelMap[R.raw.silero_vad]!!,
                turnDetectionModelPath = modelMap[R.raw.smart_turn_v3]!!,
                speakerModelPath = if (speakerModelFile.exists()) speakerModelFile.absolutePath else "",
                nativeLibDir = contextWrapper.applicationInfo.nativeLibraryDir,
                sttProvider = sttProvider.id,
                sttStreaming = sttStreaming,
                sttModelDir = sttModelDir.absolutePath,
                llmModelPath = if(useLocalLLM) {llmModelFile.absolutePath} else { "" },
                llmChatFormat = llmModelMeta.format,
                ttsProvider = ttsProvider.id,
                ttsModelDir = ttsModelDir.absolutePath,
                ttsStylePath = "",
                useLocalLLM = useLocalLLM,
                llmApiKey = BuildConfig.LLM_API_KEY,
                llmEndpoint = BuildConfig.LLM_ENDPOINT,
                llmModel = BuildConfig.LLM_MODEL,
                systemPrompt = contextWrapper.getString(R.string.default_system_prompt),
                formatInstruction = contextWrapper.getString(R.string.response_format_instruction),
                toolCallExceptionClause = contextWrapper.getString(R.string.tool_call_exception_clause),
                sonioxApiKey = BuildConfig.SONIOX_API_KEY,
                deepgramApiKey = BuildConfig.DEEPGRAM_API_KEY,
                gladiaApiKey = BuildConfig.GLADIA_API_KEY,
                cartesiaApiKey = BuildConfig.CARTESIA_API_KEY,
                cartesiaVoiceId = BuildConfig.CARTESIA_VOICE_ID,
                fishaudioApiKey = BuildConfig.FISHAUDIO_API_KEY,
                fishaudioVoiceId = BuildConfig.FISHAUDIO_VOICE_ID,
                gradiumApiKey = BuildConfig.GRADIUM_API_KEY,
                gradiumVoiceId = BuildConfig.GRADIUM_VOICE_ID,
                caBundlePath = caBundlePath
            )
            return isNativeInitialized
        }
    }

    fun start(): Boolean {
        synchronized(VasdkEngine::class.java) {
            if (isNativeStarted) return true
            isNativeStarted = nativeStart()
            return isNativeStarted
        }
    }

    fun trigger() {
        synchronized(VasdkEngine::class.java) {
            if (isNativeStarted) nativeTrigger()
        }
    }

    /** Interrupts any in-flight response and drops the LLM's conversation history, starting a fresh conversation without re-initializing the pipeline. */
    fun resetConversation() {
        synchronized(VasdkEngine::class.java) {
            if (isNativeInitialized) nativeResetConversation()
        }
    }

    /**
     * Snapshot of the currently active model for the "model info" screen. The system
     * prompt/tools/raw-prompt come from native as a JSON blob (they reflect the
     * transport's actual state); the model name and chat format are already known on
     * the Kotlin side.
     */
    fun getModelInfo(): ModelInfo {
        val json = if (isNativeInitialized) nativeGetPromptPreview() else ""
        val obj = if (json.isNotEmpty()) JSONObject(json) else null
        val systemPrompt = obj?.optString("systemPrompt", "") ?: ""
        val rawPrompt = obj?.optString("rawPrompt", "") ?: ""
        val tools = obj?.optJSONArray("tools")?.let { toolsArray ->
            (0 until toolsArray.length()).map { i -> parseToolInfo(toolsArray.getJSONObject(i)) }
        } ?: emptyList()

        return if (useLocalLLM) {
            ModelInfo(
                isLocal = true,
                modelName = llmModelMeta.modelName,
                chatFormat = llmModelMeta.format,
                systemPrompt = systemPrompt,
                tools = tools,
                rawPrompt = rawPrompt
            )
        } else {
            ModelInfo(
                isLocal = false,
                modelName = BuildConfig.LLM_MODEL,
                chatFormat = "openai",
                systemPrompt = systemPrompt,
                tools = tools,
                rawPrompt = rawPrompt
            )
        }
    }

    // Tool::toJson() on the native side emits the OpenAI function-calling shape:
    // {"name", "description", "parameters": {"type", "properties": {paramName: {type, description}}, "required": [...]}}
    private fun parseToolInfo(toolObj: JSONObject): ToolInfo {
        val parametersObj = toolObj.optJSONObject("parameters")
        val properties = parametersObj?.optJSONObject("properties")
        val required = parametersObj?.optJSONArray("required")?.let { requiredArray ->
            (0 until requiredArray.length()).map { requiredArray.getString(it) }.toSet()
        } ?: emptySet()

        val params = properties?.keys()?.asSequence()?.map { paramName ->
            val paramObj = properties.getJSONObject(paramName)
            ToolParamInfo(
                name = paramName,
                type = paramObj.optString("type", "string"),
                required = paramName in required,
                description = paramObj.optString("description", "")
            )
        }?.toList() ?: emptyList()

        return ToolInfo(
            name = toolObj.optString("name", ""),
            description = toolObj.optString("description", ""),
            parameters = params
        )
    }

    fun stop() {
        synchronized(VasdkEngine::class.java) {
            if (isNativeStarted) {
                nativeStop()
                isNativeStarted = false
            }
        }
    }

    fun release() {
        synchronized(VasdkEngine::class.java) {
            if (isNativeInitialized) {
                nativeRelease()
                isNativeInitialized = false
                isNativeStarted = false
            }
            listener = null
        }
    }

    /**
     * Extracts the embedded models into the provided directory.
     */
    fun extractEmbeddedModels(resourceId: Int, directory: File): String? {
        try {
            contextWrapper.resources.openRawResource(resourceId).use { inputStream ->
                val fileName = contextWrapper.resources.getResourceEntryName(resourceId)
                val targetFile = File(directory, fileName)
                if(targetFile.exists()) {
                    return targetFile.absolutePath
                }
                Log.d(TAG, "Extracting $fileName to ${targetFile.absolutePath}")
                FileOutputStream(targetFile).use { outputStream ->
                    inputStream.copyTo(outputStream)
                }
                return targetFile.absolutePath
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
        return null
    }

    /**
     * Writes every root CA the device currently trusts to a PEM bundle, for handing to
     * native TLS backends (sonioxpp's mbedTLS on Android) that can't see the system trust
     * store on their own. Overwrites the file on every call so it stays current.
     */
    private fun extractSystemCaBundle(directory: File): String? {
        return try {
            val trustManagerFactory = javax.net.ssl.TrustManagerFactory.getInstance(
                javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm()
            )
            trustManagerFactory.init(null as java.security.KeyStore?)
            val x509TrustManager = trustManagerFactory.trustManagers
                .filterIsInstance<javax.net.ssl.X509TrustManager>()
                .firstOrNull() ?: return null

            val bundleFile = File(directory, "system_ca_bundle.pem")
            FileOutputStream(bundleFile).use { output ->
                for (cert in x509TrustManager.acceptedIssuers) {
                    val base64 = android.util.Base64.encodeToString(cert.encoded, android.util.Base64.NO_WRAP)
                    output.write("-----BEGIN CERTIFICATE-----\n".toByteArray())
                    output.write(base64.chunked(64).joinToString("\n").toByteArray())
                    output.write("\n-----END CERTIFICATE-----\n".toByteArray())
                }
            }
            bundleFile.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract system CA bundle", e)
            null
        }
    }

    data class ModelDownloadRequest(
        val targetDir: File,
        val baseUrl: String,
        val files: List<String>
    )

    fun downloadModels(
        requests: List<ModelDownloadRequest>,
        onProgress: ((Float, Float, String) -> Unit)? = null,
        onComplete: (Boolean, List<String>) -> Unit
    ) {
        val thread = Thread {
            try {
                val outputPaths = mutableListOf<String>()
                val totalFiles = requests.sumOf { it.files.size }
                var downloadedFiles = 0
                Log.i(TAG, "downloadModels: Starting download thread for $totalFiles files.")

                requests.forEach { request ->
                    request.targetDir.mkdirs()
                    request.files.forEach { fileName ->
                        val destination = File(request.targetDir, fileName)
                        outputPaths.add(destination.absolutePath)

                        if (destination.exists()) {
                            Log.i(TAG, "File $fileName already exists, skipping download.")
                            downloadedFiles++
                            val overallProgress = downloadedFiles.toFloat() / totalFiles
                            mainHandler.post {
                                listener?.onDownloadProgress(1f, overallProgress, fileName)
                                onProgress?.invoke(1f, overallProgress, fileName)
                            }
                            return@forEach
                        }

                        Log.i(TAG, "Starting download for $fileName from ${request.baseUrl}")
                        downloadFileWithResume(
                            urlString = "${request.baseUrl}/$fileName",
                            destination = destination,
                            maxRetries = 10,
                            onProgress = { fileProgress ->
                                val overallProgress = (downloadedFiles + fileProgress) / totalFiles
                                mainHandler.post {
                                    listener?.onDownloadProgress(fileProgress, overallProgress, fileName)
                                    onProgress?.invoke(fileProgress, overallProgress, fileName)
                                }
                            }
                        )
                        Log.i(TAG, "Successfully downloaded $fileName")
                        downloadedFiles++
                    }
                }
                mainHandler.post { onComplete(true, outputPaths) }
            } catch (e: Exception) {
                Log.e(TAG, "Error downloading model files", e)
                mainHandler.post { onComplete(false, emptyList()) }
            }
        }
        thread.start()
    }

    private fun downloadFileWithResume(
        urlString: String,
        destination: File,
        maxRetries: Int,
        onProgress: (Float) -> Unit
    ) {
        val partFile = File(destination.parent, "${destination.name}.part")

        var attempt = 0
        while (attempt < maxRetries) {
            val resumeFrom = if (partFile.exists()) partFile.length() else 0L
            Log.d(TAG, "Download attempt ${attempt + 1} for $urlString (resumeFrom=$resumeFrom)")
            try {
                val conn = java.net.URL(urlString).openConnection() as java.net.HttpURLConnection
                conn.connectTimeout = 30_000
                conn.readTimeout    = 60_000
                if (resumeFrom > 0) {
                    conn.setRequestProperty("Range", "bytes=$resumeFrom-")
                }
                conn.connect()

                val responseCode = conn.responseCode
                Log.d(TAG, "HTTP Response: $responseCode for $urlString")
                if (responseCode != java.net.HttpURLConnection.HTTP_OK &&
                    responseCode != 206) {
                    Log.w(TAG, "Non-success response $responseCode for $urlString")
                    conn.disconnect()
                    throw java.io.IOException("Unexpected HTTP $responseCode")
                }

                val totalSize = if (responseCode == 206) {
                    val contentRange = conn.getHeaderField("Content-Range")
                    contentRange?.split("/")?.last()?.toLong() ?: (resumeFrom + conn.contentLength)
                } else {
                    conn.contentLength.toLong()
                }
                Log.d(TAG, "Total file size: $totalSize bytes")

                val append = responseCode == 206
                conn.inputStream.use { input ->
                    FileOutputStream(partFile, append).use { output ->
                        val buffer = ByteArray(128 * 1024)
                        var bytesRead: Int
                        var currentSize = resumeFrom
                        var lastLoggedProgress = 0L
                        while (input.read(buffer).also { bytesRead = it } != -1) {
                            output.write(buffer, 0, bytesRead)
                            currentSize += bytesRead
                            if (totalSize > 0) {
                                onProgress(currentSize.toFloat() / totalSize)
                                if (currentSize - lastLoggedProgress > 5 * 1024 * 1024) {
                                    Log.v(TAG, "Progress for ${destination.name}: $currentSize / $totalSize")
                                    lastLoggedProgress = currentSize
                                }
                            }
                        }
                    }
                }
                conn.disconnect()

                if (!partFile.renameTo(destination)) {
                    partFile.copyTo(destination, overwrite = true)
                    partFile.delete()
                }
                Log.i(TAG, "Finished downloading ${destination.name}")
                return
            } catch (e: Exception) {
                attempt++
                Log.w(TAG, "Download attempt $attempt failed for ${destination.name}: ${e.message}")
                if (attempt >= maxRetries) {
                    Log.e(TAG, "Max retries reached for ${destination.name}")
                    throw e
                }
                val sleepTime = 1000L * (1 shl minOf(attempt, 6))
                Log.i(TAG, "Retrying in ${sleepTime}ms...")
                Thread.sleep(sleepTime)
            }
        }
    }

    //  Called from native code 

    @Keep
    fun onTranscriptReceived(text: String, isFinal: Boolean) {
        mainHandler.post { listener?.onTranscriptReceived(text, isFinal) }
    }

    @Keep
    fun onStatusChanged(status: String) {
        mainHandler.post { listener?.onStatusChanged(status) }
    }

    @Keep
    fun onLlmResponse(chunk: String, isFinal: Boolean, isExpecting: Boolean, choicesJson: String) {
        mainHandler.post { listener?.onLlmResponse(chunk, isFinal, isExpecting, choicesJson) }
    }

    @Keep
    fun onAudioFeatures(source: String, features: FloatArray) {
        mainHandler.post { listener?.onAudioFeatures(source, features) }
    }

    @Keep
    fun onError(errorCode: Int, errorMessage: String) {
        mainHandler.post { listener?.onError(errorCode, errorMessage) }
    }

    // Called synchronously from a native worker thread - must NOT dispatch to main handler,
    // as C++ blocks waiting for the return value.
    @Keep
    fun onToolCall(toolName: String, argsJson: String): String {
        return listener?.onToolCall(toolName, argsJson) ?: "{\"error\":\"no listener\"}"
    }

    //  Native declarations 

    private external fun nativeInit(vadModelPath: String,
                                    turnDetectionModelPath: String,
                                    speakerModelPath: String,
                                    nativeLibDir: String,
                                    sttProvider: String,
                                    sttStreaming: Boolean,
                                    sttModelDir: String,
                                    llmModelPath: String,
                                    llmChatFormat: String,
                                    ttsProvider: String,
                                    ttsModelDir: String,
                                    ttsStylePath: String,
                                    useLocalLLM: Boolean,
                                    llmApiKey: String,
                                    llmEndpoint: String,
                                    llmModel: String,
                                    systemPrompt: String,
                                    formatInstruction: String,
                                    toolCallExceptionClause: String,
                                    sonioxApiKey: String,
                                    deepgramApiKey: String,
                                    gladiaApiKey: String,
                                    cartesiaApiKey: String,
                                    cartesiaVoiceId: String,
                                    fishaudioApiKey: String,
                                    fishaudioVoiceId: String,
                                    gradiumApiKey: String,
                                    gradiumVoiceId: String,
                                    caBundlePath: String): Boolean
    private external fun nativeStart(): Boolean
    private external fun nativeStop()
    private external fun nativeRelease()
    private external fun nativeTrigger()
    private external fun nativeResetConversation()
    private external fun nativeGetPromptPreview(): String

    companion object {
        val TAG = VasdkEngine::class.java.name
        private const val SPEAKER_MODEL_FILE = "wespeaker_en_voxceleb_resnet34_LM.onnx"
        private var instance: VasdkEngine? = null
        // Written from the init/download worker threads, read from the main thread
        @Volatile private var isNativeInitialized = false
        @Volatile private var isNativeStarted = false

        val LLAMA3_2_1B_Instruct = LLMModelMeta(
            url = "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main",
            modelName = "Llama-3.2-1B-Instruct-Q5_K_L.gguf",
            format = "llama3"
        )

        val Qwen2_5_1_5_Instruct = LLMModelMeta(
            url = "https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF/resolve/main",
            modelName = "Qwen2.5-1.5B-Instruct-Q4_0.gguf",
            format = "chatml"
        )

        val GoogleGemmaMeta = LLMModelMeta(
            url = "https://huggingface.co/bartowski/google_gemma-3-1b-it-GGUF/resolve/main",
            modelName = "google_gemma-3-1b-it-Q4_K_M.gguf",
            format = "gemma"
        )

        val LFM2_5Meta = LLMModelMeta(
            url = "https://huggingface.co/LiquidAI/LFM2.5-230M-GGUF/resolve/main",
            modelName = "LFM2.5-230M-Q8_0.gguf",
            format = "lfm2.5"
        )

        /** On-device LLM options selectable from the Settings UI when useLocalLLM is true. */
        val LOCAL_LLM_MODELS = listOf(LFM2_5Meta, LLAMA3_2_1B_Instruct, Qwen2_5_1_5_Instruct, GoogleGemmaMeta)

        @JvmStatic
        fun getInstance(context: Context): VasdkEngine {
            if (instance == null) {
                val cw = context as? ContextWrapper ?: ContextWrapper(context.applicationContext)
                instance = VasdkEngine(cw)
            }
            return instance!!
        }

        init {
            System.loadLibrary("vasdk_android")
        }
    }
}
