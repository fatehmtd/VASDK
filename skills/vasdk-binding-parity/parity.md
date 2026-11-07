# Binding parity manifest

What [`check_parity.py`](check_parity.py) reads: the C++ classes audited, one row per capability with the C function, Python and Kotlin names that expose it, and the C functions the bindings deliberately leave out. The [skill](SKILL.md) says how to fill a gap.

Rules for a row:

- The C++ cell names `Class::method` (overloads share one row), `Class` for an interface the app implements, or an exported function.
- A binding cell names what exposes it: a C function, `Class.method` in Python and Kotlin, or a function. Several are separated by commas. `—` is a gap the check reports.
- A capability no binding exposes on purpose has `—` in every binding cell and a note that starts `C++ only:` with the reason. Use that for internals an app has no use for, never to hide work.

## Audited C++

| Header | Class | Audit |
| --- | --- | --- |
| `VASDK/include/VASDK/engine/Session.h` | `Session` | methods |
| `VASDK/include/VASDK/engine/Runtime.h` | `Runtime` | methods |
| `VASDK/include/VASDK/VAEngine.h` | `VAEngine` | methods |
| `VASDK/include/VASDK/flow/FlowRegistry.h` | `FlowRegistry` | methods |
| `VASDK/include/VASDK/flow/FlowRegistry.h` | `TurnContext` | methods |
| `VASDK/include/VASDK/flow/CompiledAgent.h` | `CompiledAgent` | methods |
| `VASDK/include/VASDK/flow/FlowRuntime.h` | `FlowRuntime` | methods |
| `VASDK/include/VASDK/flow/FlowTestRunner.h` | `*` | functions |
| `VASDK/include/VASDK/engine/ProviderRegistry.h` | `ProviderRegistry` | class |
| `VASDK/include/VASDK/VADProvider.h` | `VADProvider` | class |
| `VASDK/include/VASDK/GenericTranscriptionProvider.h` | `GenericTranscriptionProvider` | class |
| `VASDK/include/VASDK/GenericVoiceSynthesisProvider.h` | `GenericVoiceSynthesisProvider` | class |
| `VASDK/include/VASDK/TurnDetectionProvider.h` | `TurnDetectionProvider` | class |
| `VASDK/include/VASDK/WakeWordProvider.h` | `WakeWordProvider` | class |
| `VASDK/include/VASDK/GenericAudioDSPProvider.h` | `GenericAudioDSPProvider` | class |
| `VASDK/include/VASDK/SpeakerEmbeddingProvider.h` | `SpeakerEmbeddingProvider` | class |
| `VASDK/include/VASDK/nlu/EmbeddingNluProvider.h` | `SentenceEncoder` | class |
| `VASDK/include/VASDK/nlu/EntityExtractor.h` | `EntityExtractor` | class |
| `VASDK/include/VASDK/llm/LLMTransport.h` | `LLMTransport` | class |
| `VASDK/include/VASDK/llm/Completion.h` | `Completion` | class |
| `VASDK/include/VASDK/context/ContextProvider.h` | `ContextProvider` | class |
| `VASDK/include/VASDK/FrameProcessor.h` | `FrameProcessor` | class |
| `VASDK/include/VASDK/GenericAudioSource.h` | `GenericAudioSource` | class |
| `VASDK/include/VASDK/GenericAudioSink.h` | `GenericAudioSink` | class |

## Capabilities

| C++ | C | Python | Kotlin | Note |
| --- | --- | --- | --- | --- |
| `Session::start` | `vasdk_session_start` | `Session.start` | `Session.start` | |
| `Session::stop` | `vasdk_session_stop` | `Session.stop` | `Session.stop` | |
| `Session::say` | `vasdk_session_send` | `Session.say` | `Session.say` | Command.say |
| `Session::injectUserText` | `vasdk_session_send` | `Session.inject_text` | `Session.injectText` | Command.inject_text |
| `Session::selectChoice` | `vasdk_session_send` | `Session.select_choice` | `Session.selectChoice` | Command.select_choice |
| `Session::interrupt` | `vasdk_session_send` | `Session.interrupt` | `Session.interrupt` | Command.interrupt |
| `Session::fireTrigger` | `vasdk_session_send` | `Session.fire_trigger` | `Session.fireTrigger` | Command.fire_trigger |
| `Session::setSessionParam` | `vasdk_session_send` | `Session.set_param` | `Session.setParam` | Command.set_session_param |
| `Session::handle` | `vasdk_session_send` | `Session.send` | `Session.send` | |
| `Session::resetConversation` | `vasdk_session_send` | `Session.reset_conversation` | `Session.resetConversation` | Command.reset_conversation |
| `Session::setBackchannel` | `vasdk_session_send` | `Session.set_backchannel` | `Session.setBackchannel` | Command.set_backchannel |
| `Session::pushToTalk` | `vasdk_session_send` | `Session.push_to_talk` | `Session.pushToTalk` | Command.push_to_talk |
| `Session::setFollowUpListenMs` | `vasdk_session_send` | `Session.set_follow_up_listen_ms` | `Session.setFollowUpListenMs` | Command.follow_up_listen_ms |
| `Session::setEntityValues` | `vasdk_session_send` | `Session.set_entity_values` | `Session.setEntityValues` | Command.set_entity_values |
| `Session::backchannel` | — | — | — | C++ only: the app knows what it set; the config's `backchannel` is where it starts |
| `Session::addListener` | `vasdk_session_on_event_message` | `Session.on_event` | `Session.onEvent` | |
| `Session::removeListener` | — | — | — | C++ only: a binding has one event callback for the session's life and filters events itself |
| `Session::registerTool` | `vasdk_session_add_tool_message` | `Session.tool` | `Session.tool` | |
| `Session::addProcessor` | `vasdk_session_add_audio_processor`, `vasdk_session_add_text_processor` | `Session.add_processor` | `Session.addProcessor` | audio slots get PCM, text slots get text |
| `Session::conversationSnapshot`, `Session::conversationSnapshotMessage` | `vasdk_session_snapshot_message` | `Session.snapshot` | `Session.snapshot` | |
| `Session::resumeConversation` | `vasdk_session_resume_message` | `Session.start` | `Session.start` | start(resume=...) |
| `Session::nextScheduledTrigger` | `vasdk_session_next_scheduled_trigger_ms` | `Session.next_scheduled_trigger` | `Session.nextScheduledTrigger` | |
| `Session::triggerState` | `vasdk_session_trigger_state` | `Session.trigger_state` | `Session.triggerState` | |
| `Session::restoreTriggerState` | `vasdk_session_restore_trigger_state` | `Session.restore_trigger_state` | `Session.restoreTriggerState` | |
| `Session::setNetworkAvailable` | `vasdk_session_set_network_available` | `Session.set_network_available` | `Session.setNetworkAvailable` | |
| `Session::usingFallbacks` | `vasdk_session_using_fallbacks` | `Session.using_fallbacks` | `Session.usingFallbacks` | |
| `Session::enrollSpeaker` | `vasdk_session_send` | `Session.enroll_speaker` | `Session.enrollSpeaker` | Command.enroll_speaker |
| `Session::addSpeaker` | `vasdk_session_send` | `Session.add_speaker` | `Session.addSpeaker` | Command.add_speaker |
| `Session::speakers` | `vasdk_session_speakers_message` | `Session.speakers` | `Session.speakers` | |
| `Session::describe` | `vasdk_session_describe` | `Session.describe` | `Session.describe` | |
| `Session::id` | — | — | — | C++ only: the app sets it (SessionOptions.id) and every event carries it (Event.session_id) |
| `Session::hasLLM` | — | — | — | C++ only: describe() says which LLM the session has |
| `Session::inputRingBuffer`, `Session::llmTransport`, `Session::toolDispatcher`, `Session::pipeline` | — | — | — | C++ only: the pipeline's internals; app-written processors and providers cover what apps do with them |
| `VAEngine::create` | `vasdk_session_create_message` | `Session.__init__` | `Session` | one session with its own providers |
| `Runtime::create` | `vasdk_runtime_create` | `Runtime.__init__` | `Runtime` | |
| `Runtime::createSession` | `vasdk_runtime_session_create` | `Runtime.session` | `Runtime.session` | |
| `Runtime::activeSessions` | `vasdk_runtime_active_sessions` | `Runtime.active_sessions` | `Runtime.activeSessions` | |
| `Runtime::idleProviderSets` | `vasdk_runtime_idle_provider_sets` | `Runtime.idle_provider_sets` | `Runtime.idleProviderSets` | |
| `Runtime::config`, `Runtime::options` | — | — | — | C++ only: what the app passed in |
| `Runtime::sentenceEncoder`, `Runtime::entityExtractor`, `Runtime::loadFallbacks`, `Runtime::acquire`, `Runtime::release`, `Runtime::configTransport`, `Runtime::createLlm` | — | — | — | C++ only: what a session asks its runtime for; bindings reach them through the engine config (webrtc, audio_input, audio_output, llm) |
| `ProviderRegistry` | `vasdk_providers_create`, `vasdk_providers_add_all` | `Providers.__init__` | `Providers.addAll` | |
| `VADProvider` | `vasdk_providers_add_vad` | `Providers.vad` | `Providers.vad` | |
| `GenericTranscriptionProvider` | `vasdk_providers_add_stt` | `Providers.stt` | `Providers.stt` | |
| `GenericVoiceSynthesisProvider` | `vasdk_providers_add_tts` | `Providers.tts` | `Providers.tts` | |
| `TurnDetectionProvider` | `vasdk_providers_add_turn` | `Providers.turn` | `Providers.turn` | |
| `WakeWordProvider` | `vasdk_providers_add_wake_word` | `Providers.wake_word` | `Providers.wakeWord` | |
| `GenericAudioDSPProvider` | `vasdk_providers_add_audio_dsp` | `Providers.audio_dsp` | `Providers.audioDsp` | |
| `SpeakerEmbeddingProvider` | `vasdk_providers_add_speaker_embedding` | `Providers.speaker_embedding` | `Providers.speakerEmbedding` | |
| `SentenceEncoder` | `vasdk_providers_add_sentence_encoder` | `Providers.sentence_encoder` | `Providers.sentenceEncoder` | |
| `EntityExtractor` | `vasdk_providers_add_entity_model`, `vasdk_registry_add_entity_extractor` | `Providers.entity_model`, `Registry.entity_extractor` | `Providers.entityModel`, `Registry.entityExtractor` | the config's entity_model; an agent's code entities |
| `LLMTransport`, `Completion` | `vasdk_providers_add_llm` | `Providers.llm` | `Providers.llm` | the same callback answers the conversation and the agent |
| `ContextProvider` | `vasdk_session_set_context_message`, `vasdk_session_on_context` | `Session.set_context`, `Session.on_context` | `Session.setContext`, `Session.onContext` | on_context: the app's state, asked each time the agent or the LLM reads it; its keys win over set_context's |
| `FrameProcessor` | `vasdk_session_add_audio_processor`, `vasdk_session_add_text_processor` | `Session.add_processor` | `Session.addProcessor` | |
| `GenericAudioSource` | `vasdk_session_push_audio` | `Session.push_audio` | `Session.pushAudio` | the app's own microphone; the device's: the config's audio_input |
| `GenericAudioSink` | `vasdk_session_on_audio` | `Session.on_audio` | `Session.onAudio` | the app's own speaker; the device's: the config's audio_output |
| `CompiledAgent::load` | `vasdk_agent_load` | `Agent.load` | `Agent.load` | |
| `CompiledAgent::compile` | `vasdk_agent_parse` | `Agent.parse` | `Agent.parse` | |
| `CompiledAgent::protoFiles` | `vasdk_agent_proto_files` | `Agent._add_proto_files` | `Agent.protoFiles` | |
| `CompiledAgent::definition`, `CompiledAgent::startPage`, `CompiledAgent::flow`, `CompiledAgent::page`, `CompiledAgent::resolve`, `CompiledAgent::rules`, `CompiledAgent::exactRules`, `CompiledAgent::knowsIntent`, `CompiledAgent::sessionParam`, `CompiledAgent::expression`, `CompiledAgent::goal`, `CompiledAgent::newGoalState`, `CompiledAgent::layers`, `CompiledAgent::entities`, `CompiledAgent::languages`, `CompiledAgent::sharedLanguages`, `CompiledAgent::registry`, `CompiledAgent::params`, `CompiledAgent::extensions` | — | — | — | C++ only: the compiled agent's tables, read by FlowRuntime; the agent file is the definition an app reads |
| `FlowRegistry::addStep` | `vasdk_registry_add_step`, `vasdk_registry_add_step_message` | `Registry.step` | `Registry.step` | |
| `FlowRegistry::addPageType` | `vasdk_registry_add_page_type`, `vasdk_registry_add_page_type_message` | `Registry.page_type` | `Registry.pageType` | |
| `FlowRegistry::addEnvelope` | `vasdk_registry_add_envelope` | `Registry.envelope` | `Registry.envelope` | |
| `FlowRegistry::addTool` | `vasdk_registry_add_tool` | `Registry.tool` | `Registry.tool` | |
| `FlowRegistry::addEntityExtractor` | `vasdk_registry_add_entity_extractor` | `Registry.entity_extractor` | `Registry.entityExtractor` | |
| `FlowRegistry::addLanguage` | `vasdk_registry_add_language`, `vasdk_language_builtin` | `Registry.add_language`, `builtin_language` | `Registry.addLanguage`, `Vasdk.builtinLanguage` | |
| `FlowRegistry::defaults`, `FlowRegistry::step`, `FlowRegistry::pageType`, `FlowRegistry::envelope`, `FlowRegistry::tool`, `FlowRegistry::entityExtractor`, `FlowRegistry::languages`, `FlowRegistry::withParams` | — | — | — | C++ only: lookups the agent compiler makes, and a C++ template helper |
| `TurnContext::text` | `vasdk_turn_text` | `Turn.text` | `Turn.text` | |
| `TurnContext::setText` | `vasdk_turn_set_text` | `Turn.set_text` | `Turn.setText` | an envelope's before redacts the words |
| `TurnContext::lookup` | `vasdk_turn_lookup_message` | `Turn.lookup` | `Turn.lookup` | |
| `TurnContext::set` | `vasdk_turn_apply` | `Turn.apply` | `Turn.apply` | StepOutcome.set |
| `TurnContext::apply` | `vasdk_turn_apply` | `Turn.apply` | `Turn.apply` | |
| `TurnContext::say` | `vasdk_turn_say` | `Turn.say` | `Turn.say` | |
| `TurnContext::ask` | `vasdk_turn_apply` | `Turn.ask` | `Turn.ask` | StepOutcome.ask |
| `TurnContext::goTo` | `vasdk_turn_goto` | `Turn.goto` | `Turn.goTo` | |
| `TurnContext::render` | `vasdk_turn_render` | `Turn.render` | `Turn.render` | |
| `TurnContext::replyText` | `vasdk_turn_reply_text` | `Turn.reply_text` | `Turn.replyText` | |
| `TurnContext::flowName` | `vasdk_turn_flow` | `Turn.flow` | `Turn.flow` | |
| `TurnContext::pageName` | `vasdk_turn_page` | `Turn.page` | `Turn.page` | |
| `TurnContext::entities` | — | — | — | C++ only: the compiled entity catalog; a step reads entity values with lookup ($nlu.entities.*) |
| `FlowRuntime::start` | `vasdk_conversation_send` | `Conversation.start` | `Conversation.start` | ConversationInput.start |
| `FlowRuntime::handleText` | `vasdk_conversation_send` | `Conversation.say` | `Conversation.say` | ConversationInput.text |
| `FlowRuntime::handleNoInput` | `vasdk_conversation_send` | `Conversation.no_input` | `Conversation.noInput` | ConversationInput.no_input |
| `FlowRuntime::selectChoice` | `vasdk_conversation_send` | `Conversation.select_choice` | `Conversation.selectChoice` | ConversationInput.select_choice |
| `FlowRuntime::fireTrigger` | `vasdk_conversation_send` | `Conversation.fire_trigger` | `Conversation.fireTrigger` | ConversationInput.fire_trigger |
| `FlowRuntime::checkTriggers` | `vasdk_conversation_send` | `Conversation.check_triggers` | `Conversation.checkTriggers` | ConversationInput.check_triggers |
| `FlowRuntime::deliverQueued` | `vasdk_conversation_send` | `Conversation.deliver_queued` | `Conversation.deliverQueued` | ConversationInput.deliver_queued |
| `FlowRuntime::started`, `FlowRuntime::ended`, `FlowRuntime::flowName`, `FlowRuntime::pageName` | `vasdk_conversation_send` | `Conversation.send` | `Conversation.send` | every ConversationReply has flow, page and ends_session |
| `FlowRuntime::noInputMs` | `vasdk_conversation_no_input_ms` | `Conversation.no_input_ms` | `Conversation.noInputMs` | |
| `FlowRuntime::replyInterrupted` | `vasdk_conversation_reply_interrupted` | `Conversation.reply_interrupted` | `Conversation.replyInterrupted` | |
| `FlowRuntime::snapshot` | `vasdk_conversation_snapshot_message` | `Conversation.snapshot` | `Conversation.snapshot` | |
| `FlowRuntime::resume` | `vasdk_conversation_resume_message` | `Conversation.resume` | `Conversation.resume` | |
| `FlowRuntime::setSessionParam` | `vasdk_conversation_set_param_message` | `Conversation.set_param` | `Conversation.setParam` | |
| `FlowRuntime::sessionParam`, `FlowRuntime::lookup` | `vasdk_conversation_lookup_message` | `Conversation.lookup` | `Conversation.lookup` | |
| `FlowRuntime::setEntityValues` | `vasdk_conversation_set_entity_values_message` | `Conversation.set_entity_values` | `Conversation.setEntityValues` | |
| `FlowRuntime::setContextSource` | `vasdk_conversation_set_context_message` | `Conversation.set_context` | `Conversation.setContext` | |
| `FlowRuntime::nextScheduledTrigger` | `vasdk_conversation_next_scheduled_trigger_ms` | `Conversation.next_scheduled_trigger` | `Conversation.nextScheduledTrigger` | |
| `FlowRuntime::triggerState` | `vasdk_conversation_trigger_state` | `Conversation.trigger_state` | `Conversation.triggerState` | |
| `FlowRuntime::restoreTriggerState` | `vasdk_conversation_restore_trigger_state` | `Conversation.restore_trigger_state` | `Conversation.restoreTriggerState` | |
| `FlowRuntime::setNlu`, `FlowRuntime::setEmbeddings`, `FlowRuntime::setEntityExtractor`, `FlowRuntime::setExactRules`, `FlowRuntime::setLlm`, `FlowRuntime::setLlmProfiles`, `FlowRuntime::hasLlm`, `FlowRuntime::setLlmHasConversation`, `FlowRuntime::setChatMinShare`, `FlowRuntime::chatMinShare` | `vasdk_session_create_message` | `Session.__init__` | `Session` | a session builds them from the engine config (understanding, llm, llm_profiles); give a session text with inject_text for the whole stack |
| `FlowRuntime::noteSaid`, `FlowRuntime::setToolCaller`, `FlowRuntime::setFillerSink`, `FlowRuntime::setLlmFillerAfterMs`, `FlowRuntime::setInterrupted`, `FlowRuntime::inactivityTriggers`, `FlowRuntime::sensitiveValues` | — | — | — | C++ only: how a Session drives its conversation (fillers, tool calls, barge-in, inactivity, masking) |
| `FlowRuntime::setClock` | — | — | — | C++ only: the C++ tests' clock; flow test files set times themselves |
| `runFlowTestFile` | `vasdk_run_test_file` | `run_test_file` | `Vasdk.runTestFile` | |
| `runFlowTest` | `vasdk_run_test_message` | `run_test` | `Vasdk.runTest` | a FlowTestCase from memory |

## C functions the bindings leave out

The C API keeps a JSON form of some calls for C, where building a proto is hard. The bindings use the proto form instead.

| C function | Covered by |
| --- | --- |
| `vasdk_conversation_start` | `vasdk_conversation_send` |
| `vasdk_conversation_say` | `vasdk_conversation_send` |
| `vasdk_conversation_no_input` | `vasdk_conversation_send` |
| `vasdk_conversation_select_choice` | `vasdk_conversation_send` |
| `vasdk_conversation_fire_trigger` | `vasdk_conversation_send` |
| `vasdk_conversation_set_param` | `vasdk_conversation_set_param_message` |
| `vasdk_conversation_lookup` | `vasdk_conversation_lookup_message` |
| `vasdk_turn_lookup` | `vasdk_turn_lookup_message` |
| `vasdk_session_create` | `vasdk_session_create_message` |
| `vasdk_session_on_event` | `vasdk_session_on_event_message` |
| `vasdk_session_resume` | `vasdk_session_resume_message` |
| `vasdk_session_snapshot` | `vasdk_session_snapshot_message` |
| `vasdk_session_add_tool` | `vasdk_session_add_tool_message` |
| `vasdk_session_set_context` | `vasdk_session_set_context_message` |
| `vasdk_session_inject_text` | `vasdk_session_send` |
| `vasdk_session_say` | `vasdk_session_send` |
| `vasdk_session_set_param` | `vasdk_session_send` |
| `vasdk_session_fire_trigger` | `vasdk_session_send` |
