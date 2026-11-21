#include "platform/audio_engine.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace platz::platform {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
} // namespace

SpatialAudioEngine::~SpatialAudioEngine() {
    Shutdown();
}

bool SpatialAudioEngine::Initialize(bool headlessSoftwareOnly, std::string* outDiagnostic) {
    Shutdown();
    sampleRateHz_ = 48000;
    hardwareActive_ = false;

    if (!headlessSoftwareOnly) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec{};
            spec.format = SDL_AUDIO_F32;
            spec.channels = 2;
            spec.freq = static_cast<int>(sampleRateHz_);

            sdlStream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (sdlStream_) {
                SDL_ResumeAudioStreamDevice(sdlStream_);
                hardwareActive_ = true;
                backendStatus_ = "SDL3 3.4.20 Hardware AudioStream (48kHz Stereo F32)";
            } else {
                backendStatus_ = std::string("Software DSP Fallback (SDL audio open note: ") + SDL_GetError() + ")";
            }
        } else {
            backendStatus_ = "Software DSP Fallback (SDL_INIT_AUDIO unavailable)";
        }
    } else {
        backendStatus_ = "Software DSP Deterministic Mode (48kHz Stereo F32)";
    }

    initialized_ = true;
    if (outDiagnostic) *outDiagnostic = backendStatus_;
    return true;
}

void SpatialAudioEngine::Shutdown() {
    if (sdlStream_) {
        SDL_DestroyAudioStream(sdlStream_);
        sdlStream_ = nullptr;
    }
    if (hardwareActive_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    activeVoices_.clear();
    hardwareActive_ = false;
    initialized_ = false;
}

void SpatialAudioEngine::SetMasterVolume(float volume) {
    masterVolume_ = std::clamp(volume, 0.0f, 1.5f);
}

bool SpatialAudioEngine::PlaySpatialCue(
    const SpatialAudioCueRequest& req,
    const glm::vec3& listenerPos,
    const glm::vec3& listenerRight,
    std::vector<float>* outRenderedStereoPcm) {
    if (!initialized_) return false;

    // Compute 3D distance attenuation and equal-power stereo pan
    float gain = std::clamp(req.volume, 0.0f, 1.5f) * (muted_ ? 0.0f : masterVolume_);
    float pan = 0.0f; // -1 = left, +1 = right
    if (req.spatial3D) {
        glm::vec3 delta = req.sourceWorldPos - listenerPos;
        float dist = glm::length(delta);
        float maxD = std::max(req.maxDistance, 1.0f);
        float atten = std::clamp(1.0f - (dist / maxD), 0.0f, 1.0f);
        atten = atten * atten;
        gain *= atten;
        if (dist > 1e-4f) {
            pan = std::clamp(glm::dot(delta / dist, glm::normalize(listenerRight)), -1.0f, 1.0f);
        }
    }

    float angle = (pan * 0.5f + 0.5f) * 1.5707963f;
    float leftGain = gain * std::cos(angle);
    float rightGain = gain * std::sin(angle);

    ActiveVoice voice{};
    voice.cueType = req.cueType;
    voice.leftGain = leftGain;
    voice.rightGain = rightGain;

    float cutoff = std::clamp(req.lowPassCutoffHz, 200.0f, 20000.0f);
    float rc = 1.0f / (kTwoPi * cutoff);
    float dt = 1.0f / static_cast<float>(sampleRateHz_);
    voice.lpAlpha = std::clamp(dt / (rc + dt), 0.01f, 1.0f);

    float pitch = std::clamp(req.pitch, 0.25f, 4.0f);
    if (req.cueType == "PhysicsImpact") {
        voice.baseFreqHz = 115.0f * pitch;
        voice.durationSec = 0.18f;
    } else if (req.cueType == "TransferArp") {
        voice.baseFreqHz = 523.25f * pitch; // C5 arpeggio
        voice.durationSec = 0.36f;
    } else if (req.cueType == "DronePulse") {
        voice.baseFreqHz = 220.0f * pitch;
        voice.durationSec = 0.28f;
    } else if (req.cueType == "UiClick") {
        voice.baseFreqHz = 880.0f * pitch;
        voice.durationSec = 0.06f;
    } else {
        voice.baseFreqHz = 659.25f * pitch; // E5 CrystalChime
        voice.durationSec = 0.30f;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeVoices_.size() >= 32) {
            activeVoices_.erase(activeVoices_.begin()); // Voice stealing cap under load
        }
        activeVoices_.push_back(voice);
        ++totalCuesPlayed_;
    }

    // Immediately synthesize and feed audio block (or capture PCM for test verification)
    uint32_t blockFrames = static_cast<uint32_t>(std::min(voice.durationSec, 0.20f) * static_cast<float>(sampleRateHz_));
    MixRealtimeBlock(blockFrames, outRenderedStereoPcm);
    return true;
}

void SpatialAudioEngine::MixRealtimeBlock(uint32_t frameCount, std::vector<float>* outInterleavedStereo) {
    if (!initialized_ || frameCount == 0) return;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<float> buf(static_cast<size_t>(frameCount) * 2, 0.0f);
    float dt = 1.0f / static_cast<float>(sampleRateHz_);
    float localPeak = 0.0f;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (sdlStream_ && hardwareActive_) {
            int queuedBytes = SDL_GetAudioStreamQueued(sdlStream_);
            if (queuedBytes == 0 && !activeVoices_.empty() && totalFramesSynthesized_ > 0) {
                ++measuredUnderrunCount_;
            }
        }

        for (auto& v : activeVoices_) {
            for (uint32_t f = 0; f < frameCount; ++f) {
                if (v.timeSec >= v.durationSec) break;
                float normT = v.timeSec / std::max(v.durationSec, 0.001f);
                float env = std::sin(std::min(normT * 10.0f, 1.0f) * 1.5707963f) * (1.0f - normT);

                float freq = v.baseFreqHz;
                if (v.cueType == "TransferArp") {
                    if (normT > 0.66f) freq *= 1.5f;
                    else if (normT > 0.33f) freq *= 1.25f;
                } else if (v.cueType == "PhysicsImpact") {
                    freq *= (1.0f - 0.55f * normT);
                }

                v.phase += kTwoPi * freq * dt;
                if (v.phase > kTwoPi) v.phase -= kTwoPi;

                float sample = std::sin(v.phase);
                if (v.cueType == "CrystalChime" || v.cueType == "TransferArp") {
                    sample = 0.72f * sample + 0.28f * std::sin(v.phase * 2.0f);
                } else if (v.cueType == "DronePulse") {
                    sample = 0.6f * sample + 0.4f * std::tanh(std::sin(v.phase * 1.5f) * 2.0f);
                }

                float rawL = sample * env * v.leftGain;
                float rawR = sample * env * v.rightGain;

                // One-pole DSP low-pass filter
                v.lpStateL += v.lpAlpha * (rawL - v.lpStateL);
                v.lpStateR += v.lpAlpha * (rawR - v.lpStateR);

                buf[f * 2 + 0] += v.lpStateL;
                buf[f * 2 + 1] += v.lpStateR;
                v.timeSec += dt;
            }
        }

        std::erase_if(activeVoices_, [](const ActiveVoice& v) {
            return v.timeSec >= v.durationSec;
        });

        for (float& s : buf) {
            s = std::clamp(s, -0.98f, 0.98f);
            localPeak = std::max(localPeak, std::abs(s));
        }

        totalFramesSynthesized_ += frameCount;
        peakOutputAmplitude_ = localPeak;

        if (sdlStream_ && hardwareActive_) {
            SDL_PutAudioStreamData(sdlStream_, buf.data(), static_cast<int>(buf.size() * sizeof(float)));
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        lastMixDurationMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    if (outInterleavedStereo) {
        *outInterleavedStereo = std::move(buf);
    }
}

AudioEngineMetrics SpatialAudioEngine::GetMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AudioEngineMetrics m{};
    m.hardwareDeviceActive = hardwareActive_;
    m.sampleRateHz = sampleRateHz_;
    m.totalFramesSynthesized = totalFramesSynthesized_;
    m.totalCuesPlayed = totalCuesPlayed_;
    m.measuredUnderrunCount = measuredUnderrunCount_;
    m.peakOutputAmplitude = peakOutputAmplitude_;
    m.lastMixDurationMs = lastMixDurationMs_;
    m.backendStatus = backendStatus_;
    return m;
}

} // namespace platz::platform
