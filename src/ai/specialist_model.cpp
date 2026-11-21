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
        {"holdoutAccuracyPct", holdoutAccuracyPct},
        {"trainingDurationMs", trainingDurationMs},
        {"avgInferenceLatencyUs", avgInferenceLatencyUs},
        {"parameterMemoryBytes", parameterMemoryBytes},
        {"convergedAndQualified", convergedAndQualified},
        {"paretoOptimal", paretoOptimal},
        {"datasetProvenanceHash", datasetProvenanceHash},
        {"holdoutSetHash", holdoutSetHash},
        {"qualificationStatus", qualificationStatus},
        {"failureOrRejectionReason", failureOrRejectionReason}
    };
}

NativeMaterialSpecialistModel::NativeMaterialSpecialistModel() = default;

void NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(
    std::vector<TrainingSample>* outTrainSet,
    std::vector<TrainingSample>* outHoldoutSet) {
    if (outTrainSet) outTrainSet->clear();
    if (outHoldoutSet) outHoldoutSet->clear();

    // 6 distinct archetype styles:
    // 0: Brushed Titanium Metal
    // 1: Polished Gold Conductor
    // 2: Matte Architectural Concrete
    // 3: Emissive PZ Cyan Energy Core
    // 4: Ceramic Dielectric Glaze
    // 5: Weathered Obsidian Basalt
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
            // Wear slightly increases roughness
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
        // Leaky ReLU activation
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

    // Save previous best weights in case this trial fails or diverges (PLZ-AIX-012)
    auto prevW1 = w1_;
    auto prevB1 = b1_;
    auto prevW2 = w2_;
    auto prevB2 = b2_;
    bool hadPreviousTrained = trained_;

    // Deterministic Xavier-like initialization from params.randomSeed
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

    auto t0 = std::chrono::high_resolution_clock::now();

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

    // Backpropagation training loop
    for (uint32_t epoch = 0; epoch < params.epochs; ++epoch) {
        for (const auto& s : trainSet) {
            // Forward
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

    auto t1 = std::chrono::high_resolution_clock::now();
    receipt.trainingDurationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    receipt.finalTrainMse = computeMse(trainSet);
    receipt.finalHoldoutMse = computeMse(holdoutSet);

    // Measure average inference latency across holdout set
    auto infT0 = std::chrono::high_resolution_clock::now();
    size_t accurateOutputs = 0;
    size_t totalOutputs = holdoutSet.size() * kOutDim;
    for (const auto& s : holdoutSet) {
        auto pred = Forward(s.features);
        for (size_t k = 0; k < kOutDim; ++k) {
            if (std::abs(pred[k] - s.targetPbr[k]) < 0.12f) {
                ++accurateOutputs;
            }
        }
    }
    auto infT1 = std::chrono::high_resolution_clock::now();
    double totalInfUs = std::chrono::duration<double, std::micro>(infT1 - infT0).count();
    receipt.avgInferenceLatencyUs = holdoutSet.empty() ? 0.0 : (totalInfUs / static_cast<double>(holdoutSet.size()));
    receipt.holdoutAccuracyPct = (totalOutputs > 0)
        ? (100.0f * static_cast<float>(accurateOutputs) / static_cast<float>(totalOutputs))
        : 0.0f;

    bool improved = (receipt.finalHoldoutMse < receipt.initialHoldoutMse * 0.35f) &&
                    (receipt.holdoutAccuracyPct >= 80.0f);

    if (!improved) {
        receipt.convergedAndQualified = false;
        receipt.qualificationStatus = "FAILED_CONVERGENCE_GATE";
        receipt.failureOrRejectionReason = "Candidate holdout MSE (" + std::to_string(receipt.finalHoldoutMse) +
                                           ") did not meet promotion threshold; recorded in negative result memory (PLZ-AIX-012)";
        if (hadPreviousTrained) {
            w1_ = prevW1; b1_ = prevB1; w2_ = prevW2; b2_ = prevB2;
        }
        failedTrialMemory_.push_back(receipt);
        return receipt;
    }

    receipt.convergedAndQualified = true;
    receipt.paretoOptimal = (!hadPreviousTrained) || (receipt.finalHoldoutMse <= activeReceipt_.finalHoldoutMse);
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

} // namespace platz::ai
