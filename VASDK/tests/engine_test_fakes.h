// Fake transport and providers for VAEngine tests: no audio devices, no models.
#pragma once

#include <VASDK/FrameProcessor.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/Transport.h>
#include <VASDK/VADProvider.h>
#include <VASDK/engine/ProviderRegistry.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fakes {

using namespace va;

class FakeTransport : public transport::AudioTransport {
public:
    std::string getName() const override { return "FakeTransport"; }
    std::string getDescription() const override { return "test"; }
    bool initialize() override { return true; }
    bool start() override { setState(transport::TransportState::CONNECTED); return true; }
    void stop() override { setState(transport::TransportState::DISCONNECTED); }
    void release() override {}
    bool sendFrame(std::shared_ptr<transport::Frame>) override { return true; }
    void setInputAudioConfig(const audio::AudioConfig&) override {}
    void setOutputAudioConfig(const audio::AudioConfig&) override {}
    void setFrameSize(size_t samples) override { frameSize = samples; }
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> buffer) override { ringBuffer = buffer; }
    size_t frameSize = 0;
    std::shared_ptr<AudioRingBuffer> ringBuffer;
};

class FakeVad : public audio::VADProvider {
public:
    bool initialize(const audio::AudioConfig&, int) override { return true; }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char*, size_t, size_t) const override { return {}; }
};

class FakeStt : public audio::GenericTranscriptionProvider {
public:
    bool initializeTranscriptionProvider() override { return true; }
    void releaseTranscriptionProvider() override {}
    void setAudioConfiguration(const audio::AudioConfig& config) override { _config = config; }
    audio::AudioConfig getAudioConfiguration() const override { return _config; }
    size_t writeAudioData(const uint8_t*, size_t size) override { return size; }
    void setTranscriptionResultListener(audio::TranscriptionResultListener* listener) override { _listener = listener; }
    audio::TranscriptionResultListener* getTranscriptionResultListener() const override { return _listener; }
    bool startTranscriptionProvider() override { _running = true; return true; }
    void stopTranscriptionProvider() override { _running = false; }
    bool isTranscriptionProviderRunning() const override { return _running; }

private:
    audio::AudioConfig _config{};
    audio::TranscriptionResultListener* _listener = nullptr;
    bool _running = false;
};

// Records what it is asked to say; a worker plays 100 ms of audio per call, like a local TTS.
class FakeTts : public audio::GenericVoiceSynthesisProvider {
public:
    ~FakeTts() override { stopVoiceSynthesisProvider(); }
    bool synthesizeSpeech(const std::string& text) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _received.push_back(text);
        _pending.push_back(text);
        _wake.notify_all();
        return true;
    }
    void cancelSynthesis() override { std::lock_guard<std::mutex> lock(_mutex); _pending.clear(); }
    bool startVoiceSynthesisProvider() override {
        _running = true;
        _worker = std::thread([this] { loop(); });
        return true;
    }
    void stopVoiceSynthesisProvider() override {
        { std::lock_guard<std::mutex> lock(_mutex); if (!_running) return; _running = false; }
        _wake.notify_all();
        if (_worker.joinable()) _worker.join();
    }
    bool isVoiceSynthesisProviderRunning() const override { return _running; }
    void setAudioConfiguration(const audio::AudioConfig& config) override { _config = config; }
    audio::AudioConfig getAudioConfiguration() const override { return _config; }
    void setVoiceSynthesisResultListener(audio::VoiceSynthesisResultListener* listener) override { _listener = listener; }
    audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override { return _listener; }

    std::vector<std::string> said() { std::lock_guard<std::mutex> lock(_mutex); return _received; }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(_mutex);
        while (true) {
            _wake.wait(lock, [this] { return !_running || !_pending.empty(); });
            if (!_running) return;
            _pending.clear();
            lock.unlock();
            std::vector<char> pcm(3200, 1);
            _listener->onSynthesisStarted();
            _listener->onAudioDataReceived(pcm.data(), pcm.size());
            _listener->onSynthesisCompleted();
            lock.lock();
        }
    }

    std::vector<std::string> _received;
    std::vector<std::string> _pending;
    audio::AudioConfig _config{};
    audio::VoiceSynthesisResultListener* _listener = nullptr;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::atomic<bool> _running{false};
    std::thread _worker;
};

class Named : public pipeline::FrameProcessor {
public:
    explicit Named(std::string name) : _name(std::move(name)) {}
    std::string getName() const override { return _name; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection direction) override {
        if (direction == pipeline::FlowDirection::DOWNSTREAM) pushDownstream(frame); else pushUpstream(frame);
    }

private:
    std::string _name;
};

// A registry with "fake" VAD, STT and TTS; every engine created from it shares `tts`.
inline engine::ProviderRegistry makeRegistry(std::shared_ptr<FakeTts> tts) {
    engine::ProviderRegistry registry;
    registry.add<audio::VADProvider>("fake", [](const engine::ProviderContext&) { return std::make_shared<FakeVad>(); });
    registry.add<audio::GenericTranscriptionProvider>("fake", [](const engine::ProviderContext&) {
        return std::make_shared<FakeStt>(); });
    registry.add<audio::GenericVoiceSynthesisProvider>("fake", [tts](const engine::ProviderContext&) { return tts; });
    return registry;
}

} // namespace fakes
