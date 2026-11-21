#pragma once

#include "ai/command_registry.h"
#include "ai/staged_session.h"
#include "core/project_state.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <json.hpp>

namespace platz::ai {

// ============================================================================
// 1. Original Local PLATZ Material Specialist Model (12x24x7 MLP)
// ============================================================================

struct TrainingSample {
    std::string sampleId;
    std::array<float, 12> features{};
    std::array<float, 7> targetPbr{};
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

    static void BuildDefaultCuratedDataset(
        std::vector<TrainingSample>* outTrainSet,
        std::vector<TrainingSample>* outHoldoutSet);

    static bool ValidateDatasetProvenanceAndConsent(
        const std::vector<TrainingSample>& dataset,
        std::string* outDatasetHash,
        std::string* outViolationReason);

    SpecialistTrialReceipt TrainAndEvaluate(
        const std::vector<TrainingSample>& trainSet,
        const std::vector<TrainingSample>& holdoutSet,
        const TrainingHyperparameters& params);

    std::array<float, 7> Forward(const std::array<float, 12>& input) const;
    MaterialPrediction PredictMaterialForStyleIndex(int styleCategoryIndex, float surfaceWear = 0.25f) const;

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
    std::vector<SpecialistTrialReceipt> failedTrialMemory_;
};

// ============================================================================
// 2. Original Local PLATZ Gameplay & Physics Balance Specialist (10x20x5 MLP)
//    Predicts (mass, restitution, friction, patrolSpan, patrolSpeed) from genre/pacing features
// ============================================================================

struct GameplayBalancePrediction {
    float mass{1.5f};
    float restitution{0.45f};
    float friction{0.40f};
    float patrolSpan{2.5f};
    float patrolSpeed{1.5f};
    std::string modelProvenance;
};

class NativeGameplayBalanceSpecialistModel {
public:
    NativeGameplayBalanceSpecialistModel();

    SpecialistTrialReceipt TrainDefaultGameplaySpecialist(uint32_t seed = 20261122u);
    GameplayBalancePrediction PredictForGenreIndex(int genreIndex, float difficultyScale = 0.5f) const;

    bool IsTrained() const { return trained_; }
    const SpecialistTrialReceipt& GetActiveReceipt() const { return activeReceipt_; }

private:
    static constexpr size_t kInDim = 10;
    static constexpr size_t kHiddenDim = 20;
    static constexpr size_t kOutDim = 5;

    std::array<float, kInDim * kHiddenDim> w1_{};
    std::array<float, kHiddenDim> b1_{};
    std::array<float, kHiddenDim * kOutDim> w2_{};
    std::array<float, kOutDim> b2_{};

    bool trained_{false};
    SpecialistTrialReceipt activeReceipt_{};
};

// ============================================================================
// 3. Coordinated Three-Role PLATZ Intelligence Workflow (PLZ-AIX-006, PLZ-AIX-010)
//    Role 1: Coordinating Planner -> Role 2: Domain Specialist -> Role 3: Critical Verifier
// ============================================================================

struct ThreeRoleWorkflowReceipt {
    bool stagedSuccessfully{false};
    uint64_t sessionToken{0};
    std::string plannerSummary;
    std::string specialistProvenance;
    std::string criticVerdict;
    uint32_t commandsStaged{0};
};

class ThreeRoleCoordinator {
public:
    static ThreeRoleWorkflowReceipt PlanSynthesizeAndVerifyEnhancement(
        const core::ProjectState& canonicalProject,
        core::ObjectId targetObjectId,
        int materialArchetypeIndex,
        int gameplayGenreIndex,
        const NativeMaterialSpecialistModel& materialSpecialist,
        const NativeGameplayBalanceSpecialistModel& gameplaySpecialist,
        StagedAiSession& targetStagingSession);
};

} // namespace platz::ai
