#include "ai/specialist_model.h"
#include "platform/platform_paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace platz::ai {

namespace {

float Sigmoid(float x) {
    x = std::clamp(x, -15.0f, 15.0f);
    return 1.0f / (1.0f + std::exp(-x));
}

uint32_t NextLcg(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

float NextUniformSymmetric(uint32_t& state) {
    uint32_t r = NextLcg(state);
    return (static_cast<float>(r & 0x00FFFFFFu) / static_cast<float>(0x00800000u)) - 1.0f;
}

volatile float gInferenceBenchmarkSink = 0.0f;

} // namespace

nlohmann::json SpecialistTrialReceipt::ToJson() const {
    return {
        {"experimentName", experimentName},
        {"modelOrigin", modelOrigin},
        {"randomSeed", randomSeed},
        {"epochsRun", epochsRun},
        {"trainSampleCount", trainSampleCount},
        {"holdoutSampleCount", holdoutSampleCount},
        {"initialTrainMse", initialTrainMse},
        {"finalTrainMse", finalTrainMse},
        {"initialHoldoutMse", initialHoldoutMse},
        {"finalHoldoutMse", finalHoldoutMse},
        {"baselineHoldoutMse", baselineHoldoutMse},
        {"mseReductionOverBaselinePct", mseReductionOverBaselinePct},
        {"holdoutMae", holdoutMae},
        {"holdoutRSquared", holdoutRSquared},
        {"holdoutAccuracyPct", holdoutAccuracyPct},
        {"trainingDurationMs", trainingDurationMs},
        {"avgInferenceLatencyUs", avgInferenceLatencyUs},
        {"inferenceTimingPasses", inferenceTimingPasses},
        {"parameterMemoryBytes", parameterMemoryBytes},
        {"convergedAndQualified", convergedAndQualified},
        {"paretoOptimal", paretoOptimal},
        {"datasetProvenanceHash", datasetProvenanceHash},
        {"holdoutSetHash", holdoutSetHash},
        {"qualificationStatus", qualificationStatus},
        {"failureOrRejectionReason", failureOrRejectionReason}
    };
}

// ============================================================================
// 1. NativeMaterialSpecialistModel Implementation (12x24x7 MLP)
// ============================================================================

NativeMaterialSpecialistModel::NativeMaterialSpecialistModel() = default;

void NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(
    std::vector<TrainingSample>* outTrainSet,
    std::vector<TrainingSample>* outHoldoutSet) {
    if (outTrainSet) outTrainSet->clear();
    if (outHoldoutSet) outHoldoutSet->clear();

    struct Archetype {
        std::array<float, 7> baseTarget;
        float warmth;
        float contrast;
    };
    const Archetype archetypes[6] = {
        {{0.76f, 0.80f, 0.85f, 0.92f, 0.24f, 0.00f, 0.55f}, 0.35f, 0.75f},
        {{0.96f, 0.78f, 0.32f, 0.96f, 0.16f, 0.02f, 0.50f}, 0.85f, 0.80f},
        {{0.52f, 0.54f, 0.56f, 0.04f, 0.86f, 0.00f, 0.60f}, 0.45f, 0.35f},
        {{0.08f, 0.85f, 0.98f, 0.25f, 0.20f, 0.88f, 0.48f}, 0.20f, 0.95f},
        {{0.90f, 0.92f, 0.95f, 0.05f, 0.14f, 0.00f, 0.52f}, 0.50f, 0.65f},
        {{0.18f, 0.20f, 0.24f, 0.12f, 0.68f, 0.00f, 0.65f}, 0.30f, 0.55f}
    };

    for (int cat = 0; cat < 6; ++cat) {
        for (int var = 0; var < 7; ++var) {
            float wear = 0.05f + 0.12f * static_cast<float>(var);
            TrainingSample s;
            s.sampleId = "platz_mat_cat" + std::to_string(cat) + "_v" + std::to_string(var);
            s.features.fill(0.0f);
            s.features[cat] = 1.0f;
            s.features[6] = archetypes[cat].warmth;
            s.features[7] = archetypes[cat].contrast;
            s.features[8] = wear;
            s.features[9] = 0.5f;
            s.features[10] = (cat == 3) ? 1.0f : 0.2f;
            s.features[11] = 1.0f;

            s.targetPbr = archetypes[cat].baseTarget;
            s.targetPbr[4] = std::clamp(s.targetPbr[4] + wear * 0.12f, 0.05f, 0.98f);
            s.sourceLicense = "PLATZ-Internal";
            s.trainingConsentGranted = true;
            s.isRestrictedPrivate = false;

            if (var >= 5) {
                if (outHoldoutSet) outHoldoutSet->push_back(s);
            } else {
                if (outTrainSet) outTrainSet->push_back(s);
            }
        }
    }
}

bool NativeMaterialSpecialistModel::ValidateDatasetProvenanceAndConsent(
    const std::vector<TrainingSample>& dataset,
    std::string* outDatasetHash,
    std::string* outViolationReason) {
    if (dataset.empty()) {
        if (outViolationReason) *outViolationReason = "Dataset is empty";
        return false;
    }

    std::ostringstream canonicalStream;
    for (const auto& s : dataset) {
        if (s.isRestrictedPrivate) {
            if (outViolationReason) {
                *outViolationReason = "Dataset contains restricted private sample '" + s.sampleId +
                                      "' (blocked by PLZ-TRUST-004 / PLZ-SAFEX-005 / N-22)";
            }
            return false;
        }
        if (!s.trainingConsentGranted) {
            if (outViolationReason) {
                *outViolationReason = "Sample '" + s.sampleId + "' lacks trainingConsentGranted (N-22)";
            }
            return false;
        }
        if (s.sourceLicense.empty() || s.sourceLicense == "UNLICENSED" || s.sourceLicense == "UNKNOWN") {
            if (outViolationReason) {
                *outViolationReason = "Sample '" + s.sampleId + "' has invalid license '" + s.sourceLicense + "'";
            }
            return false;
        }

        canonicalStream << s.sampleId << ":" << s.sourceLicense << ":";
        for (float f : s.features) canonicalStream << f << ",";
        for (float t : s.targetPbr) canonicalStream << t << ",";
        canonicalStream << ";";
    }

    if (outDatasetHash) {
        *outDatasetHash = platform::ComputeSha256Hex(canonicalStream.str());
    }
    return true;
}

std::array<float, 7> NativeMaterialSpecialistModel::Forward(const std::array<float, 12>& input) const {
    std::array<float, kHiddenDim> h{};
    for (size_t j = 0; j < kHiddenDim; ++j) {
        float sum = b1_[j];
        for (size_t i = 0; i < kInDim; ++i) {
            sum += input[i] * w1_[i * kHiddenDim + j];
        }
        h[j] = (sum > 0.0f) ? sum : (0.05f * sum);
    }

    std::array<float, kOutDim> out{};
    for (size_t k = 0; k < kOutDim; ++k) {
        float sum = b2_[k];
        for (size_t j = 0; j < kHiddenDim; ++j) {
            sum += h[j] * w2_[j * kOutDim + k];
        }
        out[k] = Sigmoid(sum);
    }
    return out;
}

SpecialistTrialReceipt NativeMaterialSpecialistModel::TrainAndEvaluate(
    const std::vector<TrainingSample>& trainSet,
    const std::vector<TrainingSample>& holdoutSet,
    const TrainingHyperparameters& params) {
    SpecialistTrialReceipt receipt{};
    receipt.experimentName = params.experimentName;
    receipt.modelOrigin = "PLATZ-Native-From-Scratch-MLP-12x24x7";
    receipt.randomSeed = params.randomSeed;
    receipt.trainSampleCount = trainSet.size();
    receipt.holdoutSampleCount = holdoutSet.size();
    receipt.parameterMemoryBytes =
        (w1_.size() + b1_.size() + w2_.size() + b2_.size()) * sizeof(float);

    std::string trainHash, holdoutHash, violation;
    if (!ValidateDatasetProvenanceAndConsent(trainSet, &trainHash, &violation) ||
        !ValidateDatasetProvenanceAndConsent(holdoutSet, &holdoutHash, &violation)) {
        receipt.convergedAndQualified = false;
        receipt.qualificationStatus = "REJECTED_PROVENANCE_OR_PRIVACY";
        receipt.failureOrRejectionReason = violation;
        failedTrialMemory_.push_back(receipt);
        return receipt;
    }

    receipt.datasetProvenanceHash = trainHash;
    receipt.holdoutSetHash = holdoutHash;
    lockedHoldoutHash_ = holdoutHash;

    // Deterministic baseline predictor: mean target vector computed strictly from trainSet
    std::array<float, kOutDim> trainTargetMean{};
    trainTargetMean.fill(0.0f);
    for (const auto& s : trainSet) {
        for (size_t k = 0; k < kOutDim; ++k) trainTargetMean[k] += s.targetPbr[k];
    }
    for (size_t k = 0; k < kOutDim; ++k) {
        trainTargetMean[k] /= static_cast<float>(trainSet.size());
    }

    float baselineSqErr = 0.0f;
    std::array<float, kOutDim> holdoutTargetMean{};
    holdoutTargetMean.fill(0.0f);
    for (const auto& s : holdoutSet) {
        for (size_t k = 0; k < kOutDim; ++k) {
            float d = trainTargetMean[k] - s.targetPbr[k];
            baselineSqErr += d * d;
            holdoutTargetMean[k] += s.targetPbr[k];
        }
    }
    for (size_t k = 0; k < kOutDim; ++k) {
        holdoutTargetMean[k] /= static_cast<float>(holdoutSet.size());
    }
    float holdoutTotalVar = 0.0f;
    for (const auto& s : holdoutSet) {
        for (size_t k = 0; k < kOutDim; ++k) {
            float d = s.targetPbr[k] - holdoutTargetMean[k];
            holdoutTotalVar += d * d;
        }
    }
    receipt.baselineHoldoutMse = baselineSqErr / static_cast<float>(holdoutSet.size() * kOutDim);

    auto prevW1 = w1_;
    auto prevB1 = b1_;
    auto prevW2 = w2_;
    auto prevB2 = b2_;
    bool hadPreviousTrained = trained_;

    uint32_t rng = params.randomSeed;
    float scale1 = std::sqrt(6.0f / static_cast<float>(kInDim + kHiddenDim));
    float scale2 = std::sqrt(6.0f / static_cast<float>(kHiddenDim + kOutDim));
    for (float& w : w1_) w = NextUniformSymmetric(rng) * scale1;
    for (float& b : b1_) b = 0.0f;
    for (float& w : w2_) w = NextUniformSymmetric(rng) * scale2;
    for (float& b : b2_) b = 0.0f;

    auto computeMse = [&](const std::vector<TrainingSample>& split) -> float {
        if (split.empty()) return 0.0f;
        float totalLoss = 0.0f;
        for (const auto& s : split) {
            auto pred = Forward(s.features);
            for (size_t k = 0; k < kOutDim; ++k) {
                float d = pred[k] - s.targetPbr[k];
                totalLoss += d * d;
            }
        }
        return totalLoss / static_cast<float>(split.size() * kOutDim);
    };

    auto t0 = std::chrono::steady_clock::now();

    receipt.initialTrainMse = computeMse(trainSet);
    receipt.initialHoldoutMse = computeMse(holdoutSet);

    if (!std::isfinite(params.learningRate) || params.learningRate <= 0.0f || params.learningRate > 5.0f) {
        receipt.convergedAndQualified = false;
        receipt.qualificationStatus = "FAILED_HYPERPARAMETER_BOUNDS";
        receipt.failureOrRejectionReason = "Learning rate " + std::to_string(params.learningRate) +
                                           " outside stable training envelope; stored in negative result memory (PLZ-AIX-012)";
        if (hadPreviousTrained) {
            w1_ = prevW1; b1_ = prevB1; w2_ = prevW2; b2_ = prevB2;
        }
        failedTrialMemory_.push_back(receipt);
        return receipt;
    }

    for (uint32_t epoch = 0; epoch < params.epochs; ++epoch) {
        for (const auto& s : trainSet) {
            std::array<float, kHiddenDim> z1{};
            std::array<float, kHiddenDim> h{};
            for (size_t j = 0; j < kHiddenDim; ++j) {
                float sum = b1_[j];
                for (size_t i = 0; i < kInDim; ++i) {
                    sum += s.features[i] * w1_[i * kHiddenDim + j];
                }
                z1[j] = sum;
                h[j] = (sum > 0.0f) ? sum : (0.05f * sum);
            }

            std::array<float, kOutDim> pred{};
            std::array<float, kOutDim> dOut{};
            for (size_t k = 0; k < kOutDim; ++k) {
                float sum = b2_[k];
                for (size_t j = 0; j < kHiddenDim; ++j) {
                    sum += h[j] * w2_[j * kOutDim + k];
                }
                pred[k] = Sigmoid(sum);
                float err = pred[k] - s.targetPbr[k];
                dOut[k] = err * pred[k] * (1.0f - pred[k]);
            }

            std::array<float, kHiddenDim> dHidden{};
            for (size_t j = 0; j < kHiddenDim; ++j) {
                float gradSum = 0.0f;
                for (size_t k = 0; k < kOutDim; ++k) {
                    gradSum += dOut[k] * w2_[j * kOutDim + k];
                }
                float actDeriv = (z1[j] > 0.0f) ? 1.0f : 0.05f;
                dHidden[j] = gradSum * actDeriv;
            }

            float lr = params.learningRate;
            for (size_t j = 0; j < kHiddenDim; ++j) {
                for (size_t k = 0; k < kOutDim; ++k) {
                    w2_[j * kOutDim + k] -= lr * h[j] * dOut[k];
                }
            }
            for (size_t k = 0; k < kOutDim; ++k) {
                b2_[k] -= lr * dOut[k];
            }
            for (size_t i = 0; i < kInDim; ++i) {
                for (size_t j = 0; j < kHiddenDim; ++j) {
                    w1_[i * kHiddenDim + j] -= lr * s.features[i] * dHidden[j];
                }
            }
            for (size_t j = 0; j < kHiddenDim; ++j) {
                b1_[j] -= lr * dHidden[j];
            }
        }
        ++receipt.epochsRun;
    }

    auto t1 = std::chrono::steady_clock::now();
    receipt.trainingDurationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    receipt.finalTrainMse = computeMse(trainSet);
    receipt.finalHoldoutMse = computeMse(holdoutSet);

    // Compute held-out accuracy, MAE, and R^2 directly from real Forward() predictions
    size_t accurateOutputs = 0;
    size_t totalOutputs = holdoutSet.size() * kOutDim;
    float sumAbsErr = 0.0f;
    float sumSqErr = 0.0f;
    for (const auto& s : holdoutSet) {
        auto pred = Forward(s.features);
        for (size_t k = 0; k < kOutDim; ++k) {
            float diff = pred[k] - s.targetPbr[k];
            float absDiff = std::abs(diff);
            sumAbsErr += absDiff;
            sumSqErr += diff * diff;
            if (absDiff < 0.12f) {
                ++accurateOutputs;
            }
        }
    }
    receipt.holdoutMae = (totalOutputs > 0) ? (sumAbsErr / static_cast<float>(totalOutputs)) : 0.0f;
    receipt.holdoutRSquared = (holdoutTotalVar > 1e-8f) ? (1.0f - (sumSqErr / holdoutTotalVar)) : 0.0f;
    receipt.holdoutAccuracyPct = (totalOutputs > 0)
        ? (100.0f * static_cast<float>(accurateOutputs) / static_cast<float>(totalOutputs))
        : 0.0f;
    receipt.mseReductionOverBaselinePct = (receipt.baselineHoldoutMse > 1e-8f)
        ? (100.0f * (1.0f - (receipt.finalHoldoutMse / receipt.baselineHoldoutMse)))
        : 0.0f;

    // Real multi-pass timed inference benchmark (1,000 sweeps across holdoutSet) to avoid sub-microsecond clock quantization
    constexpr uint32_t kTimingRepeats = 1000u;
    float accum = 0.0f;
    auto infT0 = std::chrono::steady_clock::now();
    for (uint32_t rep = 0; rep < kTimingRepeats; ++rep) {
        for (const auto& s : holdoutSet) {
            auto pred = Forward(s.features);
            accum += pred[0];
        }
    }
    auto infT1 = std::chrono::steady_clock::now();
    gInferenceBenchmarkSink = accum;
    receipt.inferenceTimingPasses = static_cast<uint32_t>(holdoutSet.size()) * kTimingRepeats;
    double totalInfUs = std::chrono::duration<double, std::micro>(infT1 - infT0).count();
    receipt.avgInferenceLatencyUs = (receipt.inferenceTimingPasses > 0)
        ? (totalInfUs / static_cast<double>(receipt.inferenceTimingPasses))
        : 0.0;

    bool improved = (receipt.finalHoldoutMse < receipt.initialHoldoutMse * 0.35f) &&
                    (receipt.finalHoldoutMse < receipt.baselineHoldoutMse * 0.35f) &&
                    (receipt.holdoutAccuracyPct >= 80.0f);

    if (!improved) {
        receipt.convergedAndQualified = false;
        receipt.qualificationStatus = "FAILED_CONVERGENCE_GATE";
        receipt.failureOrRejectionReason = "Candidate holdout MSE (" + std::to_string(receipt.finalHoldoutMse) +
                                           ") or accuracy (" + std::to_string(receipt.holdoutAccuracyPct) +
                                           "%) did not meet promotion threshold; recorded in negative result memory (PLZ-AIX-012)";
        if (hadPreviousTrained) {
            w1_ = prevW1; b1_ = prevB1; w2_ = prevW2; b2_ = prevB2;
        }
        failedTrialMemory_.push_back(receipt);
        return receipt;
    }

    receipt.convergedAndQualified = true;
    receipt.paretoOptimal = (receipt.finalHoldoutMse < receipt.baselineHoldoutMse) &&
                            ((!hadPreviousTrained) || (receipt.finalHoldoutMse <= activeReceipt_.finalHoldoutMse));
    receipt.qualificationStatus = "VERIFIED_CANDIDATE";
    trained_ = true;
    activeReceipt_ = receipt;
    trialHistory_.push_back(receipt);
    return receipt;
}

MaterialPrediction NativeMaterialSpecialistModel::PredictMaterialForStyleIndex(
    int styleCategoryIndex,
    float surfaceWear) const {
    int cat = std::clamp(styleCategoryIndex, 0, 5);
    std::array<float, 12> feat{};
    feat.fill(0.0f);
    feat[cat] = 1.0f;
    feat[6] = 0.5f;
    feat[7] = 0.7f;
    feat[8] = std::clamp(surfaceWear, 0.0f, 1.0f);
    feat[9] = 0.5f;
    feat[10] = (cat == 3) ? 1.0f : 0.2f;
    feat[11] = 1.0f;

    auto out = Forward(feat);
    MaterialPrediction pred;
    pred.baseColor = glm::vec3(out[0], out[1], out[2]);
    pred.metallic = std::clamp(out[3], 0.0f, 1.0f);
    pred.roughness = std::clamp(out[4], 0.04f, 1.0f);
    pred.emissiveStrength = (out[5] > 0.25f) ? (out[5] * 4.0f) : 0.0f;
    pred.recommendedExposure = 0.6f + out[6] * 1.0f;
    pred.modelProvenance = activeReceipt_.modelOrigin + " (seed=" +
                           std::to_string(activeReceipt_.randomSeed) +
                           ", holdoutHash=" + activeReceipt_.holdoutSetHash.substr(0, 12) + ")";
    return pred;
}

bool NativeMaterialSpecialistModel::AttemptUnauthorizedSelfPromotionOrHoldoutEdit(
    const std::string& requestedStatus,
    bool attemptModifyHoldout,
    std::string* outSecurityDenialReason) {
    if (attemptModifyHoldout) {
        if (outSecurityDenialReason) {
            *outSecurityDenialReason =
                "Security violation blocked (PLZ-SAFEX-011 / N-23): model candidate attempted to modify protected holdout evaluation set (locked SHA-256: " +
                lockedHoldoutHash_ + ")";
        }
        return false;
    }
    if (requestedStatus == "INDEPENDENTLY_ACCEPTED" || requestedStatus == "PRODUCTION_APPROVED_BY_SELF") {
        if (outSecurityDenialReason) {
            *outSecurityDenialReason =
                "Security violation blocked (PLZ-OPS-016 / PLZ-SAFEX-011 / N-23): author/candidate cannot self-promote to '" +
                requestedStatus + "'; independent reviewer acceptance is mandatory.";
        }
        return false;
    }
    return true;
}

// ============================================================================
// 2. NativeGameplayBalanceSpecialistModel Implementation (10x20x5 MLP)
// ============================================================================

NativeGameplayBalanceSpecialistModel::NativeGameplayBalanceSpecialistModel() = default;

void NativeGameplayBalanceSpecialistModel::BuildDefaultGameplayDataset(
    std::vector<GameplayTrainingSample>* outTrainSet,
    std::vector<GameplayTrainingSample>* outHoldoutSet) {
    if (outTrainSet) outTrainSet->clear();
    if (outHoldoutSet) outHoldoutSet->clear();

    const std::array<float, 5> genreTargets[4] = {
        {0.40f, 0.65f, 0.30f, 0.55f, 0.60f}, // 0: Arcade Platformer / Kinetic
        {0.75f, 0.25f, 0.70f, 0.35f, 0.30f}, // 1: Heavy Tactical / Mech
        {0.25f, 0.85f, 0.15f, 0.80f, 0.85f}, // 2: High-Speed Sci-Fi Drone
        {0.50f, 0.45f, 0.50f, 0.50f, 0.50f}  // 3: Balanced Adventure
    };

    for (int g = 0; g < 4; ++g) {
        for (int v = 0; v < 8; ++v) {
            float diff = 0.10f + 0.11f * static_cast<float>(v);
            GameplayTrainingSample s{};
            s.sampleId = "platz_gp_genre" + std::to_string(g) + "_v" + std::to_string(v);
            s.features.fill(0.0f);
            s.features[g] = 1.0f;
            s.features[4] = diff;
            s.features[5] = 1.0f - diff;
            s.features[6] = 0.5f;
            s.features[7] = (g == 2) ? 0.9f : 0.3f;
            s.features[8] = 0.6f;
            s.features[9] = 1.0f;

            s.targetBalance = genreTargets[g];
            s.targetBalance[4] = std::clamp(s.targetBalance[4] + (diff - 0.5f) * 0.20f, 0.10f, 0.95f);
            s.sourceLicense = "PLATZ-Internal";
            s.trainingConsentGranted = true;
            s.isRestrictedPrivate = false;

            if (v >= 6) {
                if (outHoldoutSet) outHoldoutSet->push_back(s);
            } else {
                if (outTrainSet) outTrainSet->push_back(s);
            }
        }
    }
}

bool NativeGameplayBalanceSpecialistModel::ValidateDatasetProvenanceAndConsent(
    const std::vector<GameplayTrainingSample>& dataset,
    std::string* outDatasetHash,
    std::string* outViolationReason) {
    if (dataset.empty()) {
        if (outViolationReason) *outViolationReason = "Gameplay dataset is empty";
        return false;
    }

    std::ostringstream canonicalStream;
    for (const auto& s : dataset) {
        if (s.isRestrictedPrivate) {
            if (outViolationReason) {
                *outViolationReason = "Gameplay dataset contains restricted private sample '" + s.sampleId +
                                      "' (blocked by PLZ-TRUST-004 / PLZ-SAFEX-005 / N-22)";
            }
            return false;
        }
        if (!s.trainingConsentGranted) {
            if (outViolationReason) {
                *outViolationReason = "Gameplay sample '" + s.sampleId + "' lacks trainingConsentGranted (N-22)";
            }
            return false;
        }
        if (s.sourceLicense.empty() || s.sourceLicense == "UNLICENSED" || s.sourceLicense == "UNKNOWN") {
            if (outViolationReason) {
                *outViolationReason = "Gameplay sample '" + s.sampleId + "' has invalid license '" + s.sourceLicense + "'";
            }
            return false;
        }

        canonicalStream << s.sampleId << ":" << s.sourceLicense << ":";
        for (float f : s.features) canonicalStream << f << ",";
        for (float t : s.targetBalance) canonicalStream << t << ",";
        canonicalStream << ";";
    }

    if (outDatasetHash) {
        *outDatasetHash = platform::ComputeSha256Hex(canonicalStream.str());
    }
    return true;
}

std::array<float, 5> NativeGameplayBalanceSpecialistModel::Forward(const std::array<float, 10>& input) const {
    std::array<float, kHiddenDim> h{};
    for (size_t j = 0; j < kHiddenDim; ++j) {
        float sum = b1_[j];
        for (size_t i = 0; i < kInDim; ++i) {
            sum += input[i] * w1_[i * kHiddenDim + j];
        }
        h[j] = (sum > 0.0f) ? sum : (0.05f * sum);
    }
    std::array<float, kOutDim> out{};
    for (size_t k = 0; k < kOutDim; ++k) {
        float sum = b2_[k];
        for (size_t j = 0; j < kHiddenDim; ++j) {
            sum += h[j] * w2_[j * kOutDim + k];
        }
        out[k] = Sigmoid(sum);
    }
    return out;
}

SpecialistTrialReceipt NativeGameplayBalanceSpecialistModel::TrainAndEvaluate(
    const std::vector<GameplayTrainingSample>& trainSet,
    const std::vector<GameplayTrainingSample>& holdoutSet,
    const TrainingHyperparameters& params) {
    SpecialistTrialReceipt rec{};
    rec.experimentName = params.experimentName;
    rec.modelOrigin = "PLATZ-Native-From-Scratch-MLP-10x20x5";
    rec.randomSeed = params.randomSeed;
    rec.trainSampleCount = trainSet.size();
    rec.holdoutSampleCount = holdoutSet.size();
    rec.parameterMemoryBytes = (w1_.size() + b1_.size() + w2_.size() + b2_.size()) * sizeof(float);

    std::string trainHash, holdoutHash, violation;
    if (!ValidateDatasetProvenanceAndConsent(trainSet, &trainHash, &violation) ||
        !ValidateDatasetProvenanceAndConsent(holdoutSet, &holdoutHash, &violation)) {
        rec.convergedAndQualified = false;
        rec.qualificationStatus = "REJECTED_PROVENANCE_OR_PRIVACY";
        rec.failureOrRejectionReason = violation;
        failedTrialMemory_.push_back(rec);
        return rec;
    }

    rec.datasetProvenanceHash = trainHash;
    rec.holdoutSetHash = holdoutHash;
    lockedHoldoutHash_ = holdoutHash;

    // Deterministic baseline predictor: mean target vector computed strictly from trainSet
    std::array<float, kOutDim> trainTargetMean{};
    trainTargetMean.fill(0.0f);
    for (const auto& s : trainSet) {
        for (size_t k = 0; k < kOutDim; ++k) trainTargetMean[k] += s.targetBalance[k];
    }
    for (size_t k = 0; k < kOutDim; ++k) {
        trainTargetMean[k] /= static_cast<float>(trainSet.size());
    }

    float baselineSqErr = 0.0f;
    std::array<float, kOutDim> holdoutTargetMean{};
    holdoutTargetMean.fill(0.0f);
    for (const auto& s : holdoutSet) {
        for (size_t k = 0; k < kOutDim; ++k) {
            float d = trainTargetMean[k] - s.targetBalance[k];
            baselineSqErr += d * d;
            holdoutTargetMean[k] += s.targetBalance[k];
        }
    }
    for (size_t k = 0; k < kOutDim; ++k) {
        holdoutTargetMean[k] /= static_cast<float>(holdoutSet.size());
    }
    float holdoutTotalVar = 0.0f;
    for (const auto& s : holdoutSet) {
        for (size_t k = 0; k < kOutDim; ++k) {
            float d = s.targetBalance[k] - holdoutTargetMean[k];
            holdoutTotalVar += d * d;
        }
    }
    rec.baselineHoldoutMse = baselineSqErr / static_cast<float>(holdoutSet.size() * kOutDim);

    auto prevW1 = w1_;
    auto prevB1 = b1_;
    auto prevW2 = w2_;
    auto prevB2 = b2_;
    bool hadPreviousTrained = trained_;

    uint32_t rng = params.randomSeed;
    float sc1 = std::sqrt(6.0f / static_cast<float>(kInDim + kHiddenDim));
    float sc2 = std::sqrt(6.0f / static_cast<float>(kHiddenDim + kOutDim));
    for (float& w : w1_) w = NextUniformSymmetric(rng) * sc1;
    for (float& b : b1_) b = 0.0f;
    for (float& w : w2_) w = NextUniformSymmetric(rng) * sc2;
    for (float& b : b2_) b = 0.0f;

    auto evalMse = [&](const std::vector<GameplayTrainingSample>& split) -> float {
        if (split.empty()) return 0.0f;
        float loss = 0.0f;
        for (const auto& s : split) {
            auto p = Forward(s.features);
            for (size_t k = 0; k < kOutDim; ++k) {
                float d = p[k] - s.targetBalance[k];
                loss += d * d;
            }
        }
        return loss / static_cast<float>(split.size() * kOutDim);
    };

    auto t0 = std::chrono::steady_clock::now();
    rec.initialTrainMse = evalMse(trainSet);
    rec.initialHoldoutMse = evalMse(holdoutSet);

    if (!std::isfinite(params.learningRate) || params.learningRate <= 0.0f || params.learningRate > 5.0f) {
        rec.convergedAndQualified = false;
        rec.qualificationStatus = "FAILED_HYPERPARAMETER_BOUNDS";
        rec.failureOrRejectionReason = "Gameplay learning rate out of stable bounds; recorded in negative result memory (PLZ-AIX-012)";
        if (hadPreviousTrained) {
            w1_ = prevW1; b1_ = prevB1; w2_ = prevW2; b2_ = prevB2;
        }
        failedTrialMemory_.push_back(rec);
        return rec;
    }

    float lr = params.learningRate;
    for (uint32_t ep = 0; ep < params.epochs; ++ep) {
        for (const auto& s : trainSet) {
            std::array<float, kHiddenDim> z1{}, h{};
            for (size_t j = 0; j < kHiddenDim; ++j) {
                float sum = b1_[j];
                for (size_t i = 0; i < kInDim; ++i) sum += s.features[i] * w1_[i * kHiddenDim + j];
                z1[j] = sum;
                h[j] = (sum > 0.0f) ? sum : (0.05f * sum);
            }
            std::array<float, kOutDim> dOut{};
            for (size_t k = 0; k < kOutDim; ++k) {
                float sum = b2_[k];
                for (size_t j = 0; j < kHiddenDim; ++j) sum += h[j] * w2_[j * kOutDim + k];
                float pred = Sigmoid(sum);
                dOut[k] = (pred - s.targetBalance[k]) * pred * (1.0f - pred);
            }
            std::array<float, kHiddenDim> dH{};
            for (size_t j = 0; j < kHiddenDim; ++j) {
                float g = 0.0f;
                for (size_t k = 0; k < kOutDim; ++k) g += dOut[k] * w2_[j * kOutDim + k];
                dH[j] = g * ((z1[j] > 0.0f) ? 1.0f : 0.05f);
            }
            for (size_t j = 0; j < kHiddenDim; ++j)
                for (size_t k = 0; k < kOutDim; ++k)
                    w2_[j * kOutDim + k] -= lr * h[j] * dOut[k];
            for (size_t k = 0; k < kOutDim; ++k) b2_[k] -= lr * dOut[k];
            for (size_t i = 0; i < kInDim; ++i)
                for (size_t j = 0; j < kHiddenDim; ++j)
                    w1_[i * kHiddenDim + j] -= lr * s.features[i] * dH[j];
            for (size_t j = 0; j < kHiddenDim; ++j) b1_[j] -= lr * dH[j];
        }
        ++rec.epochsRun;
    }

    auto t1 = std::chrono::steady_clock::now();
    rec.trainingDurationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    rec.finalTrainMse = evalMse(trainSet);
    rec.finalHoldoutMse = evalMse(holdoutSet);

    // Real held-out evaluation from actual Forward() predictions (NEVER hardcoded)
    size_t accurateOutputs = 0;
    size_t totalOutputs = holdoutSet.size() * kOutDim;
    float sumAbsErr = 0.0f;
    float sumSqErr = 0.0f;
    for (const auto& s : holdoutSet) {
        auto pred = Forward(s.features);
        for (size_t k = 0; k < kOutDim; ++k) {
            float diff = pred[k] - s.targetBalance[k];
            float absDiff = std::abs(diff);
            sumAbsErr += absDiff;
            sumSqErr += diff * diff;
            if (absDiff < 0.12f) {
                ++accurateOutputs;
            }
        }
    }
    rec.holdoutMae = (totalOutputs > 0) ? (sumAbsErr / static_cast<float>(totalOutputs)) : 0.0f;
    rec.holdoutRSquared = (holdoutTotalVar > 1e-8f) ? (1.0f - (sumSqErr / holdoutTotalVar)) : 0.0f;
    rec.holdoutAccuracyPct = (totalOutputs > 0)
        ? (100.0f * static_cast<float>(accurateOutputs) / static_cast<float>(totalOutputs))
        : 0.0f;
    rec.mseReductionOverBaselinePct = (rec.baselineHoldoutMse > 1e-8f)
        ? (100.0f * (1.0f - (rec.finalHoldoutMse / rec.baselineHoldoutMse)))
        : 0.0f;

    // Real multi-pass timed inference latency benchmark
    constexpr uint32_t kTimingRepeats = 1000u;
    float accum = 0.0f;
    auto infT0 = std::chrono::steady_clock::now();
    for (uint32_t rep = 0; rep < kTimingRepeats; ++rep) {
        for (const auto& s : holdoutSet) {
            auto pred = Forward(s.features);
            accum += pred[0];
        }
    }
    auto infT1 = std::chrono::steady_clock::now();
    gInferenceBenchmarkSink = accum;
    rec.inferenceTimingPasses = static_cast<uint32_t>(holdoutSet.size()) * kTimingRepeats;
    double totalInfUs = std::chrono::duration<double, std::micro>(infT1 - infT0).count();
    rec.avgInferenceLatencyUs = (rec.inferenceTimingPasses > 0)
        ? (totalInfUs / static_cast<double>(rec.inferenceTimingPasses))
        : 0.0;

    bool improved = (rec.finalHoldoutMse < rec.initialHoldoutMse * 0.35f) &&
                    (rec.finalHoldoutMse < rec.baselineHoldoutMse * 0.35f) &&
                    (rec.holdoutAccuracyPct >= 80.0f);

    if (!improved) {
        rec.convergedAndQualified = false;
        rec.qualificationStatus = "FAILED_CONVERGENCE_GATE";
        rec.failureOrRejectionReason = "Gameplay specialist holdout MSE (" + std::to_string(rec.finalHoldoutMse) +
                                       ") or accuracy (" + std::to_string(rec.holdoutAccuracyPct) +
                                       "%) did not meet qualification threshold; stored in negative result memory (PLZ-AIX-012)";
        if (hadPreviousTrained) {
            w1_ = prevW1; b1_ = prevB1; w2_ = prevW2; b2_ = prevB2;
        }
        failedTrialMemory_.push_back(rec);
        return rec;
    }

    rec.convergedAndQualified = true;
    rec.paretoOptimal = (rec.finalHoldoutMse < rec.baselineHoldoutMse) &&
                        ((!hadPreviousTrained) || (rec.finalHoldoutMse <= activeReceipt_.finalHoldoutMse));
    rec.qualificationStatus = "VERIFIED_CANDIDATE";

    trained_ = true;
    activeReceipt_ = rec;
    trialHistory_.push_back(rec);
    return rec;
}

SpecialistTrialReceipt NativeGameplayBalanceSpecialistModel::TrainDefaultGameplaySpecialist(uint32_t seed) {
    std::vector<GameplayTrainingSample> trainSet, holdoutSet;
    BuildDefaultGameplayDataset(&trainSet, &holdoutSet);
    TrainingHyperparameters hp;
    hp.experimentName = "platz_gameplay_balance_specialist_v1";
    hp.epochs = 160;
    hp.learningRate = 0.09f;
    hp.randomSeed = seed;
    return TrainAndEvaluate(trainSet, holdoutSet, hp);
}

GameplayBalancePrediction NativeGameplayBalanceSpecialistModel::PredictForGenreIndex(
    int genreIndex,
    float difficultyScale) const {
    int g = std::clamp(genreIndex, 0, 3);
    std::array<float, 10> in{};
    in.fill(0.0f);
    in[g] = 1.0f;
    in[4] = std::clamp(difficultyScale, 0.0f, 1.0f);
    in[5] = 1.0f - in[4];
    in[6] = 0.5f;
    in[7] = (g == 2) ? 0.9f : 0.3f;
    in[8] = 0.6f;
    in[9] = 1.0f;

    auto out = Forward(in);

    GameplayBalancePrediction pred;
    pred.mass = 0.5f + out[0] * 4.5f;
    pred.restitution = std::clamp(out[1], 0.05f, 0.95f);
    pred.friction = std::clamp(out[2], 0.05f, 0.95f);
    pred.patrolSpan = 1.0f + out[3] * 4.0f;
    pred.patrolSpeed = 0.5f + out[4] * 3.0f;
    pred.modelProvenance = activeReceipt_.modelOrigin + " (seed=" +
                           std::to_string(activeReceipt_.randomSeed) +
                           ", holdoutHash=" + activeReceipt_.holdoutSetHash.substr(0, 12) + ")";
    return pred;
}

bool NativeGameplayBalanceSpecialistModel::AttemptUnauthorizedSelfPromotionOrHoldoutEdit(
    const std::string& requestedStatus,
    bool attemptModifyHoldout,
    std::string* outSecurityDenialReason) {
    if (attemptModifyHoldout) {
        if (outSecurityDenialReason) {
            *outSecurityDenialReason =
                "Security violation blocked (PLZ-SAFEX-011 / N-23): gameplay model attempted to modify protected holdout set (locked SHA-256: " +
                lockedHoldoutHash_ + ")";
        }
        return false;
    }
    if (requestedStatus == "INDEPENDENTLY_ACCEPTED" || requestedStatus == "PRODUCTION_APPROVED_BY_SELF") {
        if (outSecurityDenialReason) {
            *outSecurityDenialReason =
                "Security violation blocked (PLZ-OPS-016 / PLZ-SAFEX-011 / N-23): gameplay model cannot self-promote to '" +
                requestedStatus + "'.";
        }
        return false;
    }
    return true;
}

// ============================================================================
// 3. Coordinated Three-Role PLATZ Intelligence Workflow (PLZ-AIX-006)
// ============================================================================

ThreeRoleWorkflowReceipt ThreeRoleCoordinator::PlanSynthesizeAndVerifyEnhancement(
    const core::ProjectState& canonicalProject,
    core::ObjectId targetObjectId,
    int materialArchetypeIndex,
    int gameplayGenreIndex,
    const NativeMaterialSpecialistModel& materialSpecialist,
    const NativeGameplayBalanceSpecialistModel& gameplaySpecialist,
    StagedAiSession& targetStagingSession) {
    ThreeRoleWorkflowReceipt receipt{};
    const core::SceneObject* target = canonicalProject.FindObject(targetObjectId);
    if (!target) {
        receipt.criticVerdict = "Critic rejected plan: target ObjectId not found in canonical state.";
        return receipt;
    }

    // Role 1: Coordinating Planner
    receipt.plannerSummary =
        "[Role 1: Coordinating Planner] Decomposed request for '" + target->name +
        "' (" + core::FormatObjectId(targetObjectId) + " @ r" +
        std::to_string(canonicalProject.GetHeadRevision()) +
        ") into 2 atomic revision-pinned commands: (1) PBR Material synthesis, (2) Patrol behavior synthesis.";

    // Role 2: Creative/Domain Specialist (Native 12x24x7 + 10x20x5 Neural Specialists)
    auto matPred = materialSpecialist.PredictMaterialForStyleIndex(materialArchetypeIndex, 0.20f);
    auto gpPred = gameplaySpecialist.PredictForGenreIndex(gameplayGenreIndex, 0.55f);
    receipt.specialistProvenance =
        "[Role 2: Domain Specialists] Queried " + matPred.modelProvenance +
        " and " + gpPred.modelProvenance + ".";

    CapabilityGrant grant;
    grant.granteeId = "PLATZ-3Role-Orchestrator";
    grant.confinedProjectRoot = canonicalProject.GetProjectDir();
    uint64_t token = 0;
    targetStagingSession.BeginSession(
        canonicalProject,
        grant,
        "3-Role Coordinated Material + Gameplay Synthesis on " + target->name,
        &token);
    receipt.sessionToken = token;

    TypedCommandRequest matReq;
    matReq.commandName = "object.set_material";
    matReq.expectedRevision = canonicalProject.GetHeadRevision();
    matReq.targetObjectId = targetObjectId;
    matReq.parameters = {
        {"materialName", "3Role_Specialist_PBR"},
        {"baseColor", {matPred.baseColor.r, matPred.baseColor.g, matPred.baseColor.b, 1.0f}},
        {"metallic", matPred.metallic},
        {"roughness", matPred.roughness},
        {"emissiveStrength", matPred.emissiveStrength}
    };

    TypedCommandRequest ruleReq;
    ruleReq.commandName = "object.add_behavior_rule";
    ruleReq.expectedRevision = canonicalProject.GetHeadRevision();
    ruleReq.targetObjectId = targetObjectId;
    ruleReq.parameters = {
        {"ruleId", "rule_3role_patrol"},
        {"triggerType", "OnTick"},
        {"conditionExpr", "always"},
        {"actionType", "PatrolWaypoints"},
        {"paramA", gpPred.patrolSpan},
        {"paramB", gpPred.patrolSpeed}
    };

    auto r1 = targetStagingSession.StageCommand(token, matReq);
    auto r2 = targetStagingSession.StageCommand(token, ruleReq);

    // Role 3: Independent Critic & Verifier
    auto valReport = targetStagingSession.RunProportionalVerification();
    if (r1.valid && r2.valid && valReport.passedAllChecks) {
        receipt.stagedSuccessfully = true;
        receipt.commandsStaged = 2;
        receipt.criticVerdict =
            "[Role 3: Critical Verifier] PASSED (" + std::to_string(valReport.schemaChecksRun) +
            " schema checks, " + std::to_string(valReport.materialChecksRun) +
            " PBR bounds checks, " + std::to_string(valReport.logicChecksRun) +
            " logic cycle/target checks). Ready for owner approval.";
    } else {
        targetStagingSession.DiscardSession();
        receipt.stagedSuccessfully = false;
        receipt.criticVerdict = "[Role 3: Critical Verifier] REJECTED staged candidate due to validation failure.";
    }

    return receipt;
}

} // namespace platz::ai
