#include "VasdkBridgeProcessor.h"

#include <spdlog/spdlog.h>

void VasdkBridgeProcessor::setCallbackTarget(JavaVM *jvm, jweak ref, jmethodID onTranscript,
                                             jmethodID onStatus, jmethodID onLlmResponse,
                                             jmethodID onError) {
    _jvm = jvm;
    _callbackRef = ref;
    _onTranscript = onTranscript;
    _onStatus = onStatus;
    _onLlmResponse = onLlmResponse;
    _onError = onError;
}

std::string VasdkBridgeProcessor::buildChoicesJson(
        const std::vector<va::transport::TextFrame::Choice> &choices) {
    if (choices.empty()) return "[]";
    std::string out = "[";
    for (size_t i = 0; i < choices.size(); ++i) {
        if (i > 0) out += ',';
        out += "{\"text\":\"";
        for (char c: choices[i].text) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        out += "\"}";
    }
    out += "]";
    return out;
}

void VasdkBridgeProcessor::processFrame(std::shared_ptr<va::transport::Frame> frame,
                                        va::pipeline::FlowDirection direction) {
    if (direction == va::pipeline::FlowDirection::DOWNSTREAM) {
        switch (frame->getType()) {
            case va::transport::FrameType::TEXT: {
                handleTextFrame(std::dynamic_pointer_cast<va::transport::TextFrame>(frame));
            }
                break;
            case va::transport::FrameType::TURN_STATE:
                handleTurnStateFrame(
                        std::dynamic_pointer_cast<va::transport::TurnStateFrame>(frame));
                break;
            case va::transport::FrameType::AGENT_STATE:
                handleAgentStateFrame(
                        std::dynamic_pointer_cast<va::transport::AgentStateFrame>(frame));
                break;
            case va::transport::FrameType::AUDIO:
                handleAudioFrame(std::dynamic_pointer_cast<va::transport::AudioFrame>(frame));
                break;
            case va::transport::FrameType::TRANSCRIPTION:
                handleTranscription(
                        std::dynamic_pointer_cast<va::transport::TranscriptionFrame>(frame));
                break;
            case va::transport::FrameType::VAD_STATE:
                handleVAD(std::dynamic_pointer_cast<va::transport::VADStateFrame>(frame));
                break;
            case va::transport::FrameType::ERROR:
                handleErrorFrame(std::dynamic_pointer_cast<va::transport::ErrorFrame>(frame));
                break;
            default:
                break;
        }
    }

    // Route the frame in its original direction.
    // Upstream frames (e.g. SynthesisStateFrame) must travel upstream so that
    // AgentStateProcessor can react to them and emit SYNTHESIS_COMPLETED events.
    // Calling pushDownstream() for upstream frames was silently dropping them.
    if (direction == va::pipeline::FlowDirection::UPSTREAM) {
        pushUpstream(frame);
    } else {
        pushDownstream(frame);
    }
}

void VasdkBridgeProcessor::handleTextFrame(
        const std::shared_ptr<va::transport::TextFrame> &textFrame) {
    if (!textFrame) return;
    if (!_jvm || !_callbackRef || !_onLlmResponse) {
        spdlog::warn("[VASDK] onLlmResponse callback not wired - dropping chunk");
        return;
    }
    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-llm", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) {
        return;
    }
    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb) {
        std::string choicesJson = buildChoicesJson(textFrame->choices);
        jstring jText = env->NewStringUTF(textFrame->text.c_str());
        jstring jChoicesJson = env->NewStringUTF(choicesJson.c_str());
        env->CallVoidMethod(cb, _onLlmResponse, jText,
                            textFrame->isFinal ? JNI_TRUE : JNI_FALSE,
                            textFrame->isExpecting ? JNI_TRUE : JNI_FALSE, jChoicesJson);
        env->DeleteLocalRef(jText);
        env->DeleteLocalRef(jChoicesJson);
        env->DeleteLocalRef(cb);
    }
    if (didAttach) _jvm->DetachCurrentThread();
}

void VasdkBridgeProcessor::handleTurnStateFrame(
        const std::shared_ptr<va::transport::TurnStateFrame> &turnStateFrame) {
    if (!turnStateFrame) return;
    std::string status =
            "turn -> state: " + std::to_string(static_cast<int>(turnStateFrame->state)) +
            ", actor: " + std::to_string(static_cast<int>(turnStateFrame->actor)) + ", type: " +
            std::to_string(static_cast<int>(turnStateFrame->type)) + ", confidence: " +
            std::to_string(turnStateFrame->confidence);
    fireCallback(_onStatus, status.c_str());
}

void VasdkBridgeProcessor::handleAgentStateFrame(
        const std::shared_ptr<va::transport::AgentStateFrame> &agentStateFrame) {
    if (!agentStateFrame) return;
    // "AGENT_STATE:<currentState>:<transition>" - parsed on the Kotlin side.
    std::string status =
            "AGENT_STATE:" + va::transport::toString(agentStateFrame->currentState) + ":" +
            va::transport::toString(agentStateFrame->transition);
    fireCallback(_onStatus, status.c_str());
}

void VasdkBridgeProcessor::handleAudioFrame(const std::shared_ptr<va::transport::AudioFrame> &af) {
    if (!af) return;

    if (_audioRingBuffer && false) {
        std::vector<uint16_t> data(af->getSizeInBytes() / 2);
        int numRead = _audioRingBuffer->readAbsolute(/*absoluteOffset=*/af->ringbufferOffset,
                /*data=*/reinterpret_cast<char *>(data.data()),
                /*dstSampleOffset=*/0,
                /*numSamples=*/af->numSamples);
        if (numRead < 0) {
            spdlog::error("[VASDK] Failed to read from ring buffer at offset {}",
                          af->ringbufferOffset);
            return;
        }
        int min = data[0];
        int max = data[0];
        double mean = 0;
        for (uint16_t val: data) {
            mean += val;
            if (val < min) min = val;
            if (val > max) max = val;
        }
        mean /= static_cast<double>(data.size());
        spdlog::info("Audio frame, num bytes read: {}, min: {}, max: {}, mean: {}", numRead,
                     min, max, mean);

        // echo back to output
        {
            auto frame = std::make_shared<va::transport::SynthesisFrame>();
            frame->audioData.resize(af->getSizeInBytes());
            memcpy(frame->audioData.data(), data.data(), af->getSizeInBytes());
            frame->frameId = "[ECHO] " + af->frameId;
            pushDownstream(frame);
        }
    }
}

void VasdkBridgeProcessor::handleTranscription(
        const std::shared_ptr<va::transport::TranscriptionFrame> &tf) {
    if (!tf) return;
    emitTranscription(tf->transcriptionResult.text.c_str(), tf->isFinal);
}

void VasdkBridgeProcessor::handleVAD(const std::shared_ptr<va::transport::VADStateFrame> &vf) {
    if (!vf) return;
    const char *s = nullptr;
    switch (vf->state) {
        case va::transport::VADState::STARTING:
            s = "SPEECH_STARTING";
            break;
        case va::transport::VADState::ONGOING:
            s = "SPEECH_ONGOING";
            break;
        case va::transport::VADState::ENDED:
            s = "SPEECH_ENDED";
            break;
        default:
            break;
    }
    if (s) fireCallback(_onStatus, s);
}

void VasdkBridgeProcessor::handleErrorFrame(const std::shared_ptr<va::transport::ErrorFrame> &ef) {
    if (!ef) return;
    std::string details = ef->errorMessage;
    details += " | severity=";
    switch (ef->severity) {
        case va::transport::ErrorSeverity::WARNING:
            details += "WARNING";
            break;
        case va::transport::ErrorSeverity::ERROR:
            details += "ERROR";
            break;
        case va::transport::ErrorSeverity::FATAL:
            details += "FATAL";
            break;
    }
    if (!ef->sourceId.empty()) {
        details += " | source=";
        details += ef->sourceId;
    }
    if (!ef->frameId.empty()) {
        details += " | frame=";
        details += ef->frameId;
    }
    fireErrorCallback(ef->errorCode, details.c_str());
}

void VasdkBridgeProcessor::fireCallback(jmethodID method, const char *text) {
    if (!_jvm || !_callbackRef || !method) return;

    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-bridge", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) {
        return;
    }

    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb) {
        jstring jstr = env->NewStringUTF(text);
        env->CallVoidMethod(cb, method, jstr);
        env->DeleteLocalRef(jstr);
        env->DeleteLocalRef(cb);
    }

    if (didAttach) _jvm->DetachCurrentThread();
}

void VasdkBridgeProcessor::emitTranscription(const char *text, bool isFinal) {
    if (!_jvm || !_callbackRef) return;

    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-bridge", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) {
        return;
    }

    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb) {
        jstring jstr = env->NewStringUTF(text);
        env->CallVoidMethod(cb, _onTranscript, jstr, isFinal);
        env->DeleteLocalRef(jstr);
        env->DeleteLocalRef(cb);
    }

    if (didAttach) _jvm->DetachCurrentThread();
}

void VasdkBridgeProcessor::fireErrorCallback(int code, const char *message) {
    if (!_jvm || !_callbackRef || !_onError) return;

    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-bridge", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) {
        return;
    }

    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb) {
        jstring jstr = env->NewStringUTF(message ? message : "Unknown pipeline error");
        env->CallVoidMethod(cb, _onError, static_cast<jint>(code), jstr);
        env->DeleteLocalRef(jstr);
        env->DeleteLocalRef(cb);
    }

    if (didAttach) _jvm->DetachCurrentThread();
}
