#pragma once

// Intercepts pipeline frames and fires Java callbacks. Designed to be
// extended as more providers (VAD, STT, TTS) are added later.

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/Frame.h>
#include <VASDK/FrameProcessor.h>

#include <jni.h>
#include <memory>
#include <string>
#include <vector>

class VasdkBridgeProcessor : public va::pipeline::FrameProcessor {
public:
    std::string getName() const override { return "VasdkBridgeProcessor"; }

    void setCallbackTarget(JavaVM *jvm, jweak ref, jmethodID onTranscript, jmethodID onStatus,
                           jmethodID onLlmResponse, jmethodID onError);

    bool shouldProcess(std::shared_ptr<va::transport::Frame> frame) const override {
        return true;
    }

    void processFrame(std::shared_ptr<va::transport::Frame> frame,
                      va::pipeline::FlowDirection direction) override;

    void setAudioRingBuffer(std::shared_ptr<va::AudioRingBuffer> ringBuffer) {
        _audioRingBuffer = ringBuffer;
    }

private:
    static std::string buildChoicesJson(const std::vector<va::transport::TextFrame::Choice> &choices);

    void handleTextFrame(const std::shared_ptr<va::transport::TextFrame> &textFrame);
    void handleTurnStateFrame(const std::shared_ptr<va::transport::TurnStateFrame> &turnStateFrame);
    void handleAgentStateFrame(const std::shared_ptr<va::transport::AgentStateFrame> &agentStateFrame);
    void handleAudioFrame(const std::shared_ptr<va::transport::AudioFrame> &af);
    void handleTranscription(const std::shared_ptr<va::transport::TranscriptionFrame> &tf);
    void handleVAD(const std::shared_ptr<va::transport::VADStateFrame> &vf);
    void handleErrorFrame(const std::shared_ptr<va::transport::ErrorFrame> &ef);

    // Attach the current thread to the JVM (if needed), invoke the Java method,
    // then detach only if this call performed the attachment.
    void fireCallback(jmethodID method, const char *text);
    void emitTranscription(const char *text, bool isFinal);
    void fireErrorCallback(int code, const char *message);

    JavaVM *_jvm{nullptr};
    jweak _callbackRef{nullptr};
    jmethodID _onTranscript{nullptr};
    jmethodID _onStatus{nullptr};
    jmethodID _onLlmResponse{nullptr};
    jmethodID _onError{nullptr};
    std::shared_ptr<va::AudioRingBuffer> _audioRingBuffer;
};
