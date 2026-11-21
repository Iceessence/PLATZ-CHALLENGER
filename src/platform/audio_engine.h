#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct SDL_AudioStream;

namespace platz::platform {

struct SpatialAudioCueRequest {
    std::string cueType{"CrystalChime"}; // "CrystalChime", "PhysicsImpact", "DronePulse", "UiClick", "TransferArp"
    glm::vec3 sourceWorldPos{0.0f};
    float volume{0.7f};
    float pitch{1.0f};
    float maxDistance{25.0f};
    bool spatial3D{true};
    float lowPassCutoffHz{12000.0f};
};

struct AudioEngineMetrics {
    bool hardwareDeviceActive{false};
    uint32_t sampleRateHz{48000};
    uint64_t totalFramesSynthesized{0};
    uint64_t totalCuesPlayed{0};
    uint64_t measuredUnderrunCount{0};
    float peakOutputAmplitude{0.0f};
    double lastMixDurationMs{0.0};
    std::string backendStatus{"Uninitialized"};
};

class SpatialAudioEngine {
public:
    SpatialAudioEngine() = default;
    ~SpatialAudioEngine();

    SpatialAudioEngine(const SpatialAudioEngine&) = delete;
    SpatialAudioEngine& operator=(const SpatialAudioEngine&) = delete;

    // Initializes SDL3 audio stream if hardware output is available; falls back cleanly to software DSP mode
    bool Initialize(bool headlessSoftwareOnly = false, std::string* outDiagnostic = nullptr);
    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return masterVolume_; }
    void SetMuted(bool muted) { muted_ = muted; }
    bool IsMuted() const { return muted_; }

    // Trigger a spatialized procedural DSP sound cue relative to the active 3D camera/listener
    bool PlaySpatialCue(
        const SpatialAudioCueRequest& req,
        const glm::vec3& listenerPos,
        const glm::vec3& listenerRight,
        std::vector<float>* outRenderedStereoPcm = nullptr);

    // Stress-test DSP mix step under host load (PLZ-ARCH-008, N-15)
    void MixRealtimeBlock(uint32_t frameCount, std::vector<float>* outInterleavedStereo = nullptr);

    AudioEngineMetrics GetMetrics() const;

private:
    struct ActiveVoice {
        std::string cueType;
        float phase{0.0f};
        float timeSec{0.0f};
        float durationSec{0.35f};
        float baseFreqHz{440.0f};
        float leftGain{0.5f};
        float rightGain{0.5f};
        float lpAlpha{0.85f};
        float lpStateL{0.0f};
        float lpStateR{0.0f};
    };

    bool initialized_{false};
    bool hardwareActive_{false};
    bool muted_{false};
    float masterVolume_{0.65f};
    uint32_t sampleRateHz_{48000};
    SDL_AudioStream* sdlStream_{nullptr};

    mutable std::mutex mutex_;
    std::vector<ActiveVoice> activeVoices_;
    uint64_t totalFramesSynthesized_{0};
    uint64_t totalCuesPlayed_{0};
    uint64_t measuredUnderrunCount_{0};
    float peakOutputAmplitude_{0.0f};
    double lastMixDurationMs_{0.0};
    std::string backendStatus_{"Uninitialized"};
};

} // namespace platz::platform
