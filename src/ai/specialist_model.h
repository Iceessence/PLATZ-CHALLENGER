#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <json.hpp>

namespace platz::ai {

// Original compact PLATZ Specialist Model (PLZ-AI-007..010, PLZ-AIX-003..004, PLZ-AIX-011..012)
// Predicts optimal PBR material parameters (baseColor.rgb, metallic, roughness, emissiveStrength, exposure)
// from a 12-D scene & intent descriptor vector.

struct TrainingSample {
    std::string sampleId;
    std::array<float, 12> features{}; // [intent Category one-hot(6), lighting warmth, contrast, wear, scale, indoor/outdoor, target FPS tier]
    std::array<float, 7> targetPbr{}; // [r, g, b, metallic, roughness, emissiveStrengthNormalized, exposureNormalized]
    std::string sourceLicense{"PLATZ-Internal"};
    bool trainingConsentGranted{true};
    bool isRestrictedPrivate{false}; // Must be rejected if true (PLZ-TRUST-004, PLZ-SAFEX-005, N-22)
};

struct TrainingHyperparameters {
    std::string experimentName{"platz_pbr_specialist_v1"};
    uint32_t epochs{160};
    float learningRate{0.08f};
    uint32_t randomSeed{20261121u};
};

struct SpecialistTrialReceipt {
    std::string experimentName;
    std::string modelOrigin{"PLATZ-Native-From-Scratch-MLP-12x24x7"};
    uint32_t randomSeed{0};
    uint32_t epochsRun{0};
    size_t trainSampleCount{0};
    size_t holdoutSampleCount{0};
    float initialTrainMse{0.0f};
    float finalTrainMse{0.0f};
    float initialHoldoutMse{0.0f};
    float finalHoldoutMse{0.0f};
    float holdoutAccuracyPct{0.0f};
    double trainingDurationMs{0.0};
    double avgInferenceLatencyUs{0.0};
    uint64_t parameterMemoryBytes{0};
    bool convergedAndQualified{false};
    bool paretoOptimal{false};
    std::string datasetProvenanceHash;
    std::string holdoutSetHash;
    std::string qualificationStatus{"VERIFIED_CANDIDATE"}; // Never INDEPENDENTLY_ACCEPTED via self-promotion (N-23)
    std::string failureOrRejectionReason;

    nlohmann::json ToJson() const;
};

struct MaterialPrediction {
    glm::vec3 baseColor{0.8f};
    float metallic{0.2f};
    float roughness{0.4f};
    float emissiveStrength{0.0f};
    float recommendedExposure{1.15f};
    std::string modelProvenance;
};

class NativeMaterialSpecialistModel {
public:
    NativeMaterialSpecialistModel();

    // Builds the default rights-cleared PLATZ internal dataset (32 train + 10 protected holdout samples)
    static void BuildDefaultCuratedDataset(
        std::vector<TrainingSample>* outTrainSet,
        std::vector<TrainingSample>* outHoldoutSet);

    // Validates dataset rights, consent, and privacy isolation (PLZ-AI-008, PLZ-SAFEX-005, N-22)
    static bool ValidateDatasetProvenanceAndConsent(
        const std::vector<TrainingSample>& dataset,
        std::string* outDatasetHash,
        std::string* outViolationReason);

    // Train from scratch with deterministic seed, evaluate on immutable holdout split, and record trial receipt
    SpecialistTrialReceipt TrainAndEvaluate(
        const std::vector<TrainingSample>& trainSet,
        const std::vector<TrainingSample>& holdoutSet,
        const TrainingHyperparameters& params);

    // Run forward pass inference
    std::array<float, 7> Forward(const std::array<float, 12>& input) const;
    MaterialPrediction PredictMaterialForStyleIndex(int styleCategoryIndex, float surfaceWear = 0.25f) const;

    // Attempted self-promotion or holdout tampering test hook (PLZ-SAFEX-011, N-23)
    bool AttemptUnauthorizedSelfPromotionOrHoldoutEdit(
        const std::string& requestedStatus,
        bool attemptModifyHoldout,
        std::string* outSecurityDenialReason);

    bool IsTrained() const { return trained_; }
    const SpecialistTrialReceipt& GetActiveReceipt() const { return activeReceipt_; }
    const std::vector<SpecialistTrialReceipt>& GetTrialHistory() const { return trialHistory_; }
    const std::vector<SpecialistTrialReceipt>& GetNegativeResultMemory() const { return failedTrialMemory_; }

private:
    static constexpr size_t kInDim = 12;
    static constexpr size_t kHiddenDim = 24;
    static constexpr size_t kOutDim = 7;

    std::array<float, kInDim * kHiddenDim> w1_{};
    std::array<float, kHiddenDim> b1_{};
    std::array<float, kHiddenDim * kOutDim> w2_{};
    std::array<float, kOutDim> b2_{};

    bool trained_{false};
    std::string lockedHoldoutHash_;
    SpecialistTrialReceipt activeReceipt_{};
    std::vector<SpecialistTrialReceipt> trialHistory_;
    std::vector<SpecialistTrialReceipt> failedTrialMemory_; // Negative result memory (PLZ-AIX-012)
};

} // namespace platz::ai
