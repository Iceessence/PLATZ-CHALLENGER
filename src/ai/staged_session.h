#pragma once

#include "ai/command_registry.h"
#include "engine/runtime/runtime_session.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace platz::ai {

enum class StagedSessionStatus {
    Idle,
    ActiveStaging,
    Committed,
    Discarded,
    StoppedAndRevoked // Triggered by Emergency Stop (PLZ-SAFEX-002, N-18)
};

struct StagedActionItem {
    uint32_t actionIndex{0};
    TypedCommandRequest request;
    core::ObjectDiffRecord diff;
    bool selectedForApproval{true};
    std::string humanReadableSummary;
};

struct StagedValidationReport {
    bool passedAllChecks{true};
    uint32_t schemaChecksRun{0};
    uint32_t logicChecksRun{0};
    uint32_t materialChecksRun{0};
    std::vector<std::string> diagnostics;
};

class StagedAiSession {
public:
    explicit StagedAiSession(const CommandRegistry& registry);

    // Begin an isolated staging branch pinned to canonicalProject.GetHeadRevision() (PLZ-AI-003, PLZ-AIX-002)
    bool BeginSession(
        const core::ProjectState& canonicalProject,
        const CapabilityGrant& grant,
        const std::string& sessionGoal,
        uint64_t* outSessionToken = nullptr);

    // Stage a command into the working preview without touching canonicalProject
    CommandValidationResult StageCommand(
        uint64_t sessionToken,
        const TypedCommandRequest& request);

    // Run proportional verification on the staged branch before review (PLZ-SAFEX-008)
    StagedValidationReport RunProportionalVerification() const;

    // Emergency Stop: immediately revokes session authority and blocks any late/async commits (PLZ-SAFEX-002, N-18)
    void EmergencyStopSession();

    // Discard staged changes leaving canonicalProject untouched
    void DiscardSession();

    // Commit selected (or all) staged actions atomically to canonicalProject if revision and validation pass
    bool ApproveAndCommit(
        uint64_t sessionToken,
        core::ProjectState& canonicalProject,
        uint64_t* outCommittedUndoBatchId = nullptr,
        std::string* outError = nullptr);

    StagedSessionStatus GetStatus() const { return status_; }
    uint64_t GetActiveSessionToken() const { return activeToken_; }
    core::RevisionId GetPinnedRevision() const { return pinnedRevision_; }
    const std::string& GetSessionGoal() const { return sessionGoal_; }
    const std::vector<StagedActionItem>& GetStagedActions() const { return stagedActions_; }
    std::vector<StagedActionItem>& GetStagedActionsMutable() { return stagedActions_; }
    const std::vector<std::string>& GetAuditTrail() const { return auditTrail_; }

private:
    const CommandRegistry& registry_;
    StagedSessionStatus status_{StagedSessionStatus::Idle};
    std::atomic<uint64_t> revocationEpoch_{1};
    uint64_t activeToken_{0};
    core::RevisionId pinnedRevision_{0};
    CapabilityGrant activeGrant_{};
    std::string sessionGoal_;

    std::vector<core::SceneObject> previewObjects_;
    std::vector<StagedActionItem> stagedActions_;
    std::vector<std::string> auditTrail_;
};

} // namespace platz::ai
