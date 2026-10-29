package com.vasdk.android

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.view.WindowCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.vasdk.android.services.ModelDownloadService
import com.vasdk.android.tools.CameraTools
import com.vasdk.android.tools.CommunicationTools
import com.vasdk.android.tools.ConfirmationSignal
import com.vasdk.android.tools.LocationTools
import com.vasdk.android.tools.MediaTools
import com.vasdk.android.tools.NotificationTools
import com.vasdk.android.tools.SystemTools
import com.vasdk.android.tools.VasdkNotificationListener
import com.vasdk.android.tools.WebTools
import com.vasdk.android.ui.MainScreen
import com.vasdk.android.ui.VasdkTheme
import org.json.JSONObject

class MainActivity : ComponentActivity(), VasdkEngine.Listener {
    private lateinit var engine: VasdkEngine
    private val viewModel: MainViewModel by viewModels()
    private val runtimePermissions = arrayOf(
        Manifest.permission.RECORD_AUDIO,
        Manifest.permission.CAMERA,
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION,
        Manifest.permission.POST_NOTIFICATIONS,
        Manifest.permission.READ_CONTACTS,
        Manifest.permission.READ_CALL_LOG,
        Manifest.permission.READ_CALENDAR,
        Manifest.permission.WRITE_CALENDAR,
        Manifest.permission.CALL_PHONE,
        Manifest.permission.SEND_SMS,
        Manifest.permission.READ_MEDIA_AUDIO,
        Manifest.permission.READ_MEDIA_IMAGES
    )

    private var micPermissionDenied = false
    private var engineReadyHandled = false
    private var micRequestAttempts = 0

    // Only the microphone is mandatory - the assistant cannot work without it. All other
    // permissions are optional: tools detect what is missing at call time and return an
    // error message the agent speaks to the user.
    private val requestPermissions: ActivityResultLauncher<Array<String>> =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (hasMicPermission()) {
                micPermissionDenied = false
                startEngineIfReady()
            } else if (micRequestAttempts < MAX_MIC_REQUEST_ATTEMPTS) {
                // The system flow can end without every group being answered - re-ask
                // for the mandatory mic alone.
                micRequestAttempts++
                requestPermissions.launch(arrayOf(Manifest.permission.RECORD_AUDIO))
            } else {
                micPermissionDenied = true
                Toast.makeText(this, getString(R.string.permission_needed), Toast.LENGTH_LONG).show()
                viewModel.onPermissionDenied()
            }
        }

    private fun hasMicPermission(): Boolean =
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private var isDownloadReceiverRegistered = false
    private val downloadReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ModelDownloadService.ACTION_DOWNLOAD_PROGRESS -> {
                    val fileProgress = intent.getFloatExtra(ModelDownloadService.EXTRA_FILE_PROGRESS, 0f)
                    val overallProgress = intent.getFloatExtra(ModelDownloadService.EXTRA_OVERALL_PROGRESS, 0f)
                    val fileName = intent.getStringExtra(ModelDownloadService.EXTRA_FILE_NAME) ?: ""
                    viewModel.onDownloadProgress(fileProgress, overallProgress, fileName)
                }
                ModelDownloadService.ACTION_MODELS_READY -> {
                    unregisterDownloadReceiver()
                    engine.setListener(this@MainActivity)
                    val success = intent.getBooleanExtra(ModelDownloadService.EXTRA_SUCCESS, false)
                    if (success) {
                        onEngineInitialized()
                    } else {
                        Toast.makeText(this@MainActivity, "Failed to download model", Toast.LENGTH_LONG).show()
                        viewModel.onInitCompleted(false)
                    }
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // The assistant is voice-driven and often has no touch interaction for long
        // stretches (listening/thinking/speaking), so the display would otherwise sleep
        // mid-conversation. The flag only takes effect while this window is on-screen, so
        // it stops applying as soon as the app leaves the foreground.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Match status/nav bar to the light theme surface colour (dark icons on top)
        window.statusBarColor = getColor(R.color.surface)
        window.navigationBarColor = getColor(R.color.surface)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            isAppearanceLightStatusBars = true
            isAppearanceLightNavigationBars = true
        }

        engine = VasdkEngine.getInstance(this)
        engine.setListener(this)
        viewModel.setInitialConfig(
            sttProvider = engine.sttProvider,
            sttStreaming = engine.sttStreaming,
            ttsProvider = engine.ttsProvider,
            useLocalLlm = engine.useLocalLLM,
            llmModelName = engine.llmModelMeta.modelName
        )
        ConfirmationSignal.setListener { awaiting -> viewModel.onAwaitingConfirmationChanged(awaiting) }

        // On a fresh install this is deferred to finishSetup(), so the wizard comes first.
        if (ProviderPreferences.isSetupCompleted(this) && !hasMicPermission()) {
            // Ask for everything up front; only the mic result is binding.
            requestPermissions.launch(runtimePermissions)
        }

        setContent {
            VasdkTheme {
                val uiState = viewModel.uiState.collectAsStateWithLifecycle().value
                MainScreen(
                    state = uiState,
                    onCancelClick = { onCancelClicked() },
                    onOpenAnimationTest = { viewModel.onOpenAnimationTestRequested() },
                    onOpenModelInfo = { viewModel.onModelInfoRequested(engine.getModelInfo()) },
                    onModelInfoDismiss = { viewModel.onModelInfoDismissed() },
                    onEnableWriteSettingsClick = { onEnableWriteSettingsClick() },
                    onEnableNotificationAccessClick = { onEnableNotificationAccessClick() },
                    onOpenSettings = { viewModel.onSettingsRequested() },
                    onNewConversation = { onNewConversationClicked() },
                    onSettingsDismiss = { viewModel.onSettingsDismissed() },
                    onSttProviderChange = { provider -> viewModel.onSettingsSttProviderChanged(provider) },
                    onSttStreamingChange = { streaming -> viewModel.onSettingsSttStreamingChanged(streaming) },
                    onTtsProviderChange = { provider -> viewModel.onSettingsTtsProviderChanged(provider) },
                    onUseLocalLlmChange = { useLocal -> viewModel.onSettingsUseLocalLlmChanged(useLocal) },
                    onLlmModelChange = { modelName -> viewModel.onSettingsLlmModelChanged(modelName) },
                    onApplyConfig = { applyPendingConfig() },
                    onRunSetup = { onRunSetupClicked() },
                    onSetupNext = { viewModel.onSetupNext() },
                    onSetupBack = { viewModel.onSetupBack() },
                    onSetupFinish = { finishSetup() }
                )
            }
        }

    }

    override fun onStart() {
        super.onStart()
        if (engine.isReady()) {
            // Models already downloaded and initialized on a previous run
            onEngineInitialized()
        } else if (!ProviderPreferences.isSetupCompleted(this)) {
            // Nothing is downloaded until the user has picked providers in the wizard.
            viewModel.onSetupRequired()
        } else {
            registerDownloadReceiver()
            startForegroundService(Intent(this, ModelDownloadService::class.java))
        }
    }

    override fun onStop() {
        super.onStop()
        unregisterDownloadReceiver()
    }

    override fun onResume() {
        super.onResume()
        if (::engine.isInitialized) {
            engine.setListener(this)
            // Safety net: recover if ACTION_MODELS_READY was missed (e.g. the activity was
            // stopped behind the permission dialogs), or start the engine after the user
            // granted the mic permission from Settings.
            if (!engineReadyHandled && engine.isReady()) {
                onEngineInitialized()
            } else {
                startEngineIfReady()
            }
        }
        viewModel.onSpecialPermissionStateRefreshed(
            canWriteSettings = Settings.System.canWrite(this),
            hasNotificationAccess = VasdkNotificationListener.instance != null
        )
    }

    override fun onPause() {
        super.onPause()
        if (::engine.isInitialized) {
            engine.setListener(null)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // A configuration change (rotation, theme, multi-window resize, locale, etc.)
        // destroys and immediately recreates the Activity. Only tear down the pipeline
        // when the Activity is genuinely finishing, otherwise every rotation would force
        // a full re-download/re-init of the engine and drop the live conversation.
        if (!isChangingConfigurations) {
            engine.stop()
            engine.release()
        }
    }

    private fun registerDownloadReceiver() {
        if (isDownloadReceiverRegistered) return
        val filter = IntentFilter().apply {
            addAction(ModelDownloadService.ACTION_MODELS_READY)
            addAction(ModelDownloadService.ACTION_DOWNLOAD_PROGRESS)
        }
        registerReceiver(downloadReceiver, filter, RECEIVER_NOT_EXPORTED)
        isDownloadReceiverRegistered = true
    }

    private fun unregisterDownloadReceiver() {
        if (!isDownloadReceiverRegistered) return
        unregisterReceiver(downloadReceiver)
        isDownloadReceiverRegistered = false
    }

    private fun onCancelClicked() {
        engine.trigger()
    }

    private fun onNewConversationClicked() {
        engine.resetConversation()
        viewModel.onNewConversationRequested()
    }

    private fun onEnableWriteSettingsClick() {
        startActivity(
            Intent(Settings.ACTION_MANAGE_WRITE_SETTINGS, Uri.parse("package:$packageName"))
        )
    }

    private fun onEnableNotificationAccessClick() {
        startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
    }

    // Re-opens the wizard from Settings. The running pipeline is paused (not released) so the
    // assistant doesn't listen behind the wizard; finishSetup() then rebuilds it. The
    // setup-completed flag stays set, so if the app is closed mid-wizard the next launch
    // simply resumes with the previous configuration.
    private fun onRunSetupClicked() {
        engine.stop()
        viewModel.onSetupRequired()
    }

    private fun finishSetup() {
        ProviderPreferences.setSetupCompleted(this, true)
        viewModel.onSetupFinished()
        if (!hasMicPermission()) {
            requestPermissions.launch(runtimePermissions)
        }
        applyPendingConfig()
    }

    private fun applyPendingConfig() {
        val state = viewModel.uiState.value
        applyConfig(
            sttProvider = state.settingsSttProvider,
            sttStreaming = state.settingsSttStreaming,
            ttsProvider = state.settingsTtsProvider,
            useLocalLlm = state.settingsUseLocalLlm,
            llmModelName = state.settingsLlmModelName
        )
    }

    // Tears down the current pipeline and re-runs the download-init-start flow used on first
    // launch, this time with the newly selected providers. ACTION_MODELS_READY (handled by
    // downloadReceiver) drives the rest, eventually reaching onEngineInitialized() again.
    private fun applyConfig(
        sttProvider: SttProvider,
        sttStreaming: Boolean,
        ttsProvider: TtsProvider,
        useLocalLlm: Boolean,
        llmModelName: String
    ) {
        engine.stop()
        engine.release()
        engine.setSttProvider(sttProvider)
        engine.setSttStreaming(sttStreaming)
        engine.setTtsProvider(ttsProvider)
        engine.setLlmMode(useLocalLlm)
        VasdkEngine.LOCAL_LLM_MODELS.firstOrNull { it.modelName == llmModelName }?.let {
            engine.setLocalLlmModel(it)
        }
        engineReadyHandled = false
        viewModel.onConfigApplyRequested()
        registerDownloadReceiver()
        startForegroundService(Intent(this, ModelDownloadService::class.java))
    }

    private fun onEngineInitialized() {
        if (engineReadyHandled) {
            startEngineIfReady()
            return
        }
        engineReadyHandled = true
        viewModel.onInitCompleted(true)
        if (micPermissionDenied) {
            viewModel.onPermissionDenied()
        } else {
            // If the permission dialog is still open, the result callback starts the engine.
            startEngineIfReady()
        }
    }

    // The engine starts once models are downloaded and initialized AND the mic is granted,
    // and stays paused while the setup wizard is open.
    private fun startEngineIfReady() {
        if (!engine.isReady() || engine.isStarted()) return
        if (!hasMicPermission()) return
        if (viewModel.uiState.value.setupWizardVisible) return
        onPermissionGranted()
    }

    private fun onPermissionGranted() {
        if (!engine.start()) {
            Toast.makeText(this, "Failed to start engine", Toast.LENGTH_SHORT).show()
            viewModel.onEngineStartFailed()
            return
        }
        viewModel.onPermissionGrantedAndEngineStarted()
    }

    override fun onTranscriptReceived(text: String, isFinal: Boolean) {
        viewModel.onTranscriptReceived(text, isFinal)
    }

    override fun onStatusChanged(status: String) {
        viewModel.onStatusChanged(status)
    }

    override fun onLlmResponse(chunk: String, isFinal: Boolean, isExpecting: Boolean, choicesJson: String) {
        viewModel.onLlmResponse(chunk, isFinal, isExpecting, choicesJson)
    }

    override fun onDownloadProgress(fileProgress: Float, overallProgress: Float, fileName: String) {
        viewModel.onDownloadProgress(fileProgress, overallProgress, fileName)
    }

    override fun onAudioFeatures(source: String, features: FloatArray) {
        viewModel.onAudioFeatures(source, features)
    }

    override fun onError(errorCode: Int, errorMessage: String) {
        viewModel.onPipelineError(errorCode, errorMessage)
    }

    // Called synchronously from a native worker thread - keep implementations fast and non-blocking.
    override fun onToolCall(toolName: String, argsJson: String): String = try {
        when {
            SystemTools.owns(toolName)       -> SystemTools.handle(this, toolName, argsJson)
            MediaTools.owns(toolName)        -> MediaTools.handle(this, toolName, argsJson)
            NotificationTools.owns(toolName) -> NotificationTools.handle(this, toolName, argsJson)
            CommunicationTools.owns(toolName)-> CommunicationTools.handle(this, toolName, argsJson)
            LocationTools.owns(toolName)     -> LocationTools.handle(this, toolName, argsJson)
            WebTools.owns(toolName)          -> WebTools.handle(this, toolName, argsJson)
            else -> JSONObject().put("error", "unknown tool: $toolName").toString()
        }
    } catch (e: Exception) {
        JSONObject().put("error", e.message ?: "unknown error").toString()
    }

    companion object {
        val TAG: String = MainActivity::class.java.name
        // One mic-only retry if the initial system flow ends without the mic being answered
        private const val MAX_MIC_REQUEST_ATTEMPTS = 1
    }
}
