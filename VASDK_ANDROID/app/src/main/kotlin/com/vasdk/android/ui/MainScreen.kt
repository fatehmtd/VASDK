package com.vasdk.android.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.vasdk.android.MainMessageRole
import com.vasdk.android.MainUiState
import com.vasdk.android.SttProvider
import com.vasdk.android.TtsProvider

@Composable
fun MainScreen(
    state: MainUiState,
    onCancelClick: () -> Unit,
    onOpenAnimationTest: () -> Unit,
    onOpenModelInfo: () -> Unit,
    onModelInfoDismiss: () -> Unit = {},
    onEnableWriteSettingsClick: () -> Unit = {},
    onEnableNotificationAccessClick: () -> Unit = {},
    onOpenSettings: () -> Unit = {},
    onNewConversation: () -> Unit = {},
    onSettingsDismiss: () -> Unit = {},
    onSttProviderChange: (SttProvider) -> Unit = {},
    onSttStreamingChange: (Boolean) -> Unit = {},
    onTtsProviderChange: (TtsProvider) -> Unit = {},
    onUseLocalLlmChange: (Boolean) -> Unit = {},
    onLlmModelChange: (String) -> Unit = {},
    onApplyConfig: () -> Unit = {},
    onRunSetup: () -> Unit = {},
    onSetupNext: () -> Unit = {},
    onSetupBack: () -> Unit = {},
    onSetupFinish: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    val listState = rememberLazyListState()

    // Re-scroll on message changes and on BottomPanel height changes (mic widget, typing
    // dots, waveform) - those resize the LazyColumn via the shared weight(1f) layout, and
    // without this the last message ends up clipped behind the taller panel.
    LaunchedEffect(
        state.messages.size,
        state.messages.lastOrNull()?.text,
        state.isSpeaking,
        state.typingVisible,
        state.waveformMode
    ) {
        if (state.messages.isNotEmpty()) {
            listState.animateScrollToItem(state.messages.lastIndex)
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(BgDark)
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            Header(
                statusText = state.statusText,
                statusActive = state.statusDotActive,
                onOpenAnimationTest = onOpenAnimationTest,
                onOpenModelInfo = onOpenModelInfo,
                onOpenSettings = onOpenSettings,
                onNewConversation = onNewConversation,
                settingsEnabled = !state.configApplyInProgress &&
                    !state.downloadOverlayVisible &&
                    !state.initOverlayVisible &&
                    !state.setupWizardVisible
            )

            PermissionsBanner(
                canWriteSettings = state.canWriteSettings,
                hasNotificationAccess = state.hasNotificationAccess,
                onEnableWriteSettingsClick = onEnableWriteSettingsClick,
                onEnableNotificationAccessClick = onEnableNotificationAccessClick
            )

            AnimatedVisibility(
                visible = state.awaitingResponse,
                enter = fadeIn(),
                exit = fadeOut()
            ) {
                ExpectationBanner(progress = state.awaitingResponseProgress)
            }

            LazyColumn(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(vertical = 12.dp),
                state = listState
            ) {
                items(state.messages, key = { it.id }) { message ->
                    when (message.role) {
                        MainMessageRole.USER -> UserMessageItem(message)
                        MainMessageRole.ASSISTANT -> AssistantMessageItem(message)
                        MainMessageRole.THINKING -> ThinkingMessageItem()
                        MainMessageRole.USER_SPEAKING -> UserSpeakingItem()
                        MainMessageRole.ERROR -> ErrorMessageItem(message)
                    }
                }
            }

            BottomPanel(state = state, onCancelClick = onCancelClick)
        }

        AnimatedVisibility(
            visible = state.downloadOverlayVisible,
            enter = fadeIn(),
            exit = fadeOut()
        ) {
            DownloadOverlay(
                fileName = state.downloadFileName,
                filePercent = state.downloadFilePercent,
                overallPercent = state.downloadOverallPercent
            )
        }

        AnimatedVisibility(
            visible = state.initOverlayVisible,
            enter = fadeIn(),
            exit = fadeOut()
        ) {
            InitOverlay()
        }

        AnimatedVisibility(
            visible = state.modelInfoVisible,
            enter = fadeIn(),
            exit = fadeOut()
        ) {
            ModelInfoOverlay(info = state.modelInfo, onCloseClick = onModelInfoDismiss)
        }

        AnimatedVisibility(
            visible = state.settingsVisible,
            enter = fadeIn(),
            exit = fadeOut()
        ) {
            SettingsOverlay(
                sttProvider = state.settingsSttProvider,
                sttStreaming = state.settingsSttStreaming,
                ttsProvider = state.settingsTtsProvider,
                useLocalLlm = state.settingsUseLocalLlm,
                llmModelName = state.settingsLlmModelName,
                applyEnabled = !state.configApplyInProgress,
                applyInProgress = state.configApplyInProgress,
                onSttProviderChange = onSttProviderChange,
                onSttStreamingChange = onSttStreamingChange,
                onTtsProviderChange = onTtsProviderChange,
                onUseLocalLlmChange = onUseLocalLlmChange,
                onLlmModelChange = onLlmModelChange,
                onApplyClick = onApplyConfig,
                onRunSetupClick = onRunSetup,
                onCloseClick = onSettingsDismiss
            )
        }

        AnimatedVisibility(
            visible = state.setupWizardVisible,
            enter = fadeIn(),
            exit = fadeOut()
        ) {
            SetupWizard(
                step = state.setupStep,
                sttProvider = state.settingsSttProvider,
                sttStreaming = state.settingsSttStreaming,
                ttsProvider = state.settingsTtsProvider,
                useLocalLlm = state.settingsUseLocalLlm,
                llmModelName = state.settingsLlmModelName,
                onSttProviderChange = onSttProviderChange,
                onSttStreamingChange = onSttStreamingChange,
                onTtsProviderChange = onTtsProviderChange,
                onUseLocalLlmChange = onUseLocalLlmChange,
                onLlmModelChange = onLlmModelChange,
                onNext = onSetupNext,
                onBack = onSetupBack,
                onFinish = onSetupFinish
            )
        }
    }
}
