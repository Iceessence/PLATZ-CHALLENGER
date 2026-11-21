#pragma once

#include "core/project_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace platz::platform {
class SpatialAudioEngine;
}

namespace platz::engine::runtime {

struct LogicDiagnostic {
    core::ObjectId objectId{0};
    std::string objectName;
    std::string ruleId;
    std::string severity{"ERROR"}; // "ERROR", "WARNING"
    std::string message;
};

// Time-inspectable visual gameplay debugging frame entry (PLZ-LOGIC-008, PLZ-CREATEX2-004)
struct RuleDebuggerTimelineEntry {
    uint64_t stepIndex{0};
    double timestampSec{0.0};
    core::ObjectId objectId{0};
    std::string objectName;
    std::string ruleId;
    std::string triggerType;
    std::string actionType;
    std::string stateBefore;
    std::string stateAfter;
    glm::vec3 positionAfter{0.0f};
    int64_t energyCrystalsAfter{0};
};

struct RuntimeExecutionMetrics {
    uint64_t stepCount{0};
    double elapsedPlayTimeSec{0.0};
    uint64_t rulesEvaluatedCount{0};
    uint64_t rulesTriggeredCount{0};
    uint64_t collisionsResolvedCount{0};
    uint64_t conservedTransfersSucceeded{0};
    uint64_t conservedTransfersRolledBack{0};
    uint64_t animationsEvaluatedCount{0};
    uint64_t audioCuesTriggeredCount{0};
};

class RuntimeSession {
public:
    RuntimeSession() = default;

    // Static validation of visual/no-code behavior rules before or during Play (PLZ-LOGIC-008, PLZ-CREATEX2-003, N-16)
    static std::vector<LogicDiagnostic> ValidateProjectLogic(const core::ProjectState& project);

    // Deterministic unified animation track evaluator (used by both Play runtime and non-destructive Editor scrub preview)
    static void EvaluateObjectAnimationAtTime(core::SceneObject& obj, double timeSec);

    void AttachAudioEngine(platform::SpatialAudioEngine* audioEngine) { audioEngine_ = audioEngine; }
    void SetAudioListener(const glm::vec3& position, const glm::vec3& rightDir) {
        listenerPos_ = position;
        listenerRight_ = rightDir;
    }

    // Start isolated Play projection from canonical editor state (PLZ-RUNTIME-001, N-07)
    bool StartPlay(const core::ProjectState& canonicalProject, std::string* outError = nullptr);

    // Stop Play and discard transient runtime mutations unless explicitly applied back (N-07)
    void StopPlay();

    // Opt-in apply-back of a specific runtime object's state into canonical authoring (PLZ-RUNTIME-001)
    bool ApplyRuntimeObjectToAuthoring(
        core::ObjectId objectId,
        core::ProjectState& canonicalProject,
        std::string* outError = nullptr) const;

    bool IsPlaying() const { return playing_; }
    bool IsPaused() const { return paused_; }
    void SetPaused(bool paused) { paused_ = paused; }

    // Deterministic fixed-timestep or variable-timestep simulation step (PLZ-GAME-004, PLZ-LOGIC-002..003)
    void StepSimulation(double deltaSeconds);

    // Trigger an explicit interaction event on an object in the running world
    bool TriggerInteraction(core::ObjectId objectId, std::string* outEventLog = nullptr);

    // Transactional conserved inventory/resource transfer (PLZ-GAME-010, PLZ-ARCH-009, N-09)
    bool ExecuteConservedResourceTransfer(
        core::ObjectId fromObjectId,
        core::ObjectId toObjectId,
        const std::string& resourceName,
        int64_t amount,
        bool simulateInterruptionMidTransfer = false,
        std::string* outError = nullptr);

    int64_t ComputeTotalResourceAcrossWorld(const std::string& resourceName) const;

    // Gameplay Save / Load Checkpoints strictly separate from Editor Revisions (PLZ-GAME-016, PLZ-RUNTIME-002, N-08)
    bool SaveGameplayCheckpoint(
        core::ProjectState& projectForDb,
        const std::string& slotName = "quicksave_1",
        std::string* outError = nullptr) const;

    bool LoadGameplayCheckpoint(
        const core::ProjectState& projectForDb,
        const std::string& slotName = "quicksave_1",
        std::string* outError = nullptr);

    const std::vector<core::SceneObject>& GetRuntimeObjects() const { return runtimeObjects_; }
    std::vector<core::SceneObject>& GetRuntimeObjectsMutable() { return runtimeObjects_; }
    core::SceneObject* FindRuntimeObject(core::ObjectId id);
    const core::SceneObject* FindRuntimeObject(core::ObjectId id) const;

    const RuntimeExecutionMetrics& GetMetrics() const { return metrics_; }
    const std::vector<std::string>& GetEventTraceLog() const { return eventTraceLog_; }
    const std::vector<RuleDebuggerTimelineEntry>& GetDebuggerTimeline() const { return debuggerTimeline_; }

private:
    void AppendTrace(const std::string& msg);
    void RecordDebuggerTimelineEntry(
        const core::SceneObject& self,
        const core::BehaviorRule& rule,
        const std::string& stateBefore);
    bool EvaluateRuleCondition(const core::SceneObject& self, const core::BehaviorRule& rule) const;
    void ExecuteRuleAction(core::SceneObject& self, const core::BehaviorRule& rule, double dt);
    void TriggerSpatialCueIfConfigured(const core::SceneObject& obj, const std::string& overrideCue = "");

    bool playing_{false};
    bool paused_{false};
    core::RevisionId sourceAuthoringRevision_{0};
    std::vector<core::SceneObject> runtimeObjects_;
    RuntimeExecutionMetrics metrics_{};
    std::vector<std::string> eventTraceLog_;
    std::vector<RuleDebuggerTimelineEntry> debuggerTimeline_;

    platform::SpatialAudioEngine* audioEngine_{nullptr};
    glm::vec3 listenerPos_{0.0f, 3.0f, 10.0f};
    glm::vec3 listenerRight_{1.0f, 0.0f, 0.0f};
};

} // namespace platz::engine::runtime
