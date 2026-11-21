#include "ai/staged_session.h"

namespace platz::ai {

StagedAiSession::StagedAiSession(const CommandRegistry& registry)
    : registry_(registry) {}

bool StagedAiSession::BeginSession(
    const core::ProjectState& canonicalProject,
    const CapabilityGrant& grant,
    const std::string& sessionGoal,
    uint64_t* outSessionToken) {
    activeToken_ = revocationEpoch_.fetch_add(1, std::memory_order_seq_cst);
    status_ = StagedSessionStatus::ActiveStaging;
    pinnedRevision_ = canonicalProject.GetHeadRevision();
    activeGrant_ = grant;
    sessionGoal_ = sessionGoal;
    previewObjects_ = canonicalProject.GetObjects();
    stagedActions_.clear();

    auditTrail_.push_back("Session #" + std::to_string(activeToken_) +
                          " started at revision r" + std::to_string(pinnedRevision_) +
                          ": " + sessionGoal_);
    if (outSessionToken) *outSessionToken = activeToken_;
    return true;
}

void StagedAiSession::EmergencyStopSession() {
    revocationEpoch_.fetch_add(1000, std::memory_order_seq_cst);
    status_ = StagedSessionStatus::StoppedAndRevoked;
    auditTrail_.push_back("[EMERGENCY STOP] Session #" + std::to_string(activeToken_) +
                          " revoked; all further or late commits blocked (N-18).");
}

void StagedAiSession::DiscardSession() {
    revocationEpoch_.fetch_add(1, std::memory_order_seq_cst);
    status_ = StagedSessionStatus::Discarded;
    stagedActions_.clear();
    previewObjects_.clear();
    auditTrail_.push_back("Session #" + std::to_string(activeToken_) + " discarded.");
}

CommandValidationResult StagedAiSession::StageCommand(
    uint64_t sessionToken,
    const TypedCommandRequest& request) {
    CommandValidationResult res{};
    if (status_ == StagedSessionStatus::StoppedAndRevoked || sessionToken != activeToken_) {
        res.permissionDenied = true;
        res.errorCode = "ERR_SESSION_STOPPED_OR_REVOKED";
        res.diagnosticMessage = "Session was stopped or token revoked; late command rejected (PLZ-SAFEX-002 / N-18)";
        auditTrail_.push_back("[BLOCKED LATE STAGE] " + res.diagnosticMessage);
        return res;
    }
    if (status_ != StagedSessionStatus::ActiveStaging) {
        res.permissionDenied = true;
        res.errorCode = "ERR_SESSION_NOT_ACTIVE";
        res.diagnosticMessage = "No active AI staging session";
        return res;
    }

    // Build a temporary lightweight ProjectState view representing the current staged preview at pinnedRevision_
    core::ProjectState tempPreview;
    for (const auto& o : previewObjects_) {
        tempPreview.GetObjectsMutable().push_back(o);
    }

    // Check command against registry with expectedRevision checked against pinnedRevision_
    TypedCommandRequest adjustedReq = request;
    if (adjustedReq.expectedRevision != pinnedRevision_) {
        res.staleRevision = true;
        res.errorCode = "ERR_STALE_REVISION";
        res.diagnosticMessage = "Command expected revision r" + std::to_string(adjustedReq.expectedRevision) +
                                " does not match session pinned revision r" + std::to_string(pinnedRevision_) + " (N-02)";
        return res;
    }
    adjustedReq.expectedRevision = tempPreview.GetHeadRevision();

    core::ObjectDiffRecord diff{};
    res = registry_.ValidateAndApplyCommand(tempPreview, activeGrant_, adjustedReq, &diff);
    if (!res.valid) {
        auditTrail_.push_back("[REJECTED COMMAND] " + request.commandName + ": " + res.diagnosticMessage);
        return res;
    }

    if (request.commandName != "project.query_object") {
        previewObjects_ = tempPreview.GetObjects();
        StagedActionItem item;
        item.actionIndex = static_cast<uint32_t>(stagedActions_.size());
        item.request = request;
        item.diff = std::move(diff);
        item.selectedForApproval = true;
        item.humanReadableSummary = request.commandName + " on " + core::FormatObjectId(item.diff.objectId);
        stagedActions_.push_back(std::move(item));
        auditTrail_.push_back("[STAGED] " + stagedActions_.back().humanReadableSummary);
    }
    return res;
}

StagedValidationReport StagedAiSession::RunProportionalVerification() const {
    StagedValidationReport rep{};
    core::ProjectState previewProject;
    previewProject.GetObjectsMutable() = previewObjects_;

    for (const auto& item : stagedActions_) {
        if (!item.selectedForApproval) continue;
        ++rep.schemaChecksRun;

        if (item.request.commandName == "object.set_material") {
            ++rep.materialChecksRun;
            const core::SceneObject* obj = previewProject.FindObject(item.diff.objectId);
            if (obj && (obj->material.roughness < 0.04f || obj->material.roughness > 1.0f ||
                        obj->material.metallic < 0.0f || obj->material.metallic > 1.0f)) {
                rep.passedAllChecks = false;
                rep.diagnostics.push_back("Material PBR bounds check failed on " + obj->name);
            }
        }
        if (item.request.commandName == "object.add_behavior_rule") {
            ++rep.logicChecksRun;
        }
    }

    auto logicDiags = engine::runtime::RuntimeSession::ValidateProjectLogic(previewProject);
    if (!logicDiags.empty()) {
        ++rep.logicChecksRun;
        for (const auto& d : logicDiags) {
            if (d.severity == "ERROR") {
                rep.passedAllChecks = false;
            }
            rep.diagnostics.push_back("[" + d.severity + "] " + d.objectName + " (" + d.ruleId + "): " + d.message);
        }
    }

    return rep;
}

bool StagedAiSession::ApproveAndCommit(
    uint64_t sessionToken,
    core::ProjectState& canonicalProject,
    uint64_t* outCommittedUndoBatchId,
    std::string* outError) {
    // 1. Check Emergency Stop / token revocation FIRST (PLZ-SAFEX-002, N-18)
    if (status_ == StagedSessionStatus::StoppedAndRevoked || sessionToken != activeToken_) {
        if (outError) *outError = "Commit denied: session was revoked by Emergency Stop (N-18)";
        auditTrail_.push_back("[BLOCKED LATE COMMIT] Session revoked by Emergency Stop (N-18)");
        return false;
    }
    if (status_ != StagedSessionStatus::ActiveStaging) {
        if (outError) *outError = "Session is not in ActiveStaging state";
        return false;
    }

    // 2. Check for stale canonical revision (N-02)
    if (canonicalProject.GetHeadRevision() != pinnedRevision_) {
        if (outError) {
            *outError = "Stale session commit denied (N-02): session pinned at r" +
                        std::to_string(pinnedRevision_) + ", but canonical project is now at r" +
                        std::to_string(canonicalProject.GetHeadRevision());
        }
        return false;
    }

    // 3. Run proportional verification on selected staged actions (PLZ-SAFEX-008)
    StagedValidationReport val = RunProportionalVerification();
    if (!val.passedAllChecks) {
        if (outError) {
            *outError = "Proportional verification failed before commit (PLZ-SAFEX-008): " +
                        (val.diagnostics.empty() ? "validation error" : val.diagnostics.front());
        }
        return false;
    }

    // 4. Apply only selected actions and record a single reversible AI undo batch (PLZ-SAFEX-009, N-25)
    std::vector<core::ObjectDiffRecord> appliedDiffs;
    for (const auto& item : stagedActions_) {
        if (!item.selectedForApproval) continue;

        core::ObjectDiffRecord d = item.diff;
        if (!d.existedBefore && d.existsAfter) {
            core::SceneObject created = core::SceneObject::FromJson(d.afterJson);
            canonicalProject.GetObjectsMutable().push_back(created);
            appliedDiffs.push_back(d);
        } else if (d.existedBefore && !d.existsAfter) {
            if (const core::SceneObject* cur = canonicalProject.FindObject(d.objectId)) {
                d.beforeJson = cur->ToJson();
                std::erase_if(canonicalProject.GetObjectsMutable(),
                    [&](const core::SceneObject& o) { return o.id == d.objectId; });
                appliedDiffs.push_back(d);
            }
        } else if (d.existedBefore && d.existsAfter) {
            if (core::SceneObject* cur = canonicalProject.FindObject(d.objectId)) {
                d.beforeJson = cur->ToJson();
                *cur = core::SceneObject::FromJson(d.afterJson);
                appliedDiffs.push_back(d);
            }
        }
    }

    uint64_t batchId = canonicalProject.RecordMultiObjectBatch(
        "AI Staged Session: " + sessionGoal_,
        "AI",
        activeGrant_.granteeId,
        appliedDiffs);

    status_ = StagedSessionStatus::Committed;
    activeToken_ = 0;
    if (outCommittedUndoBatchId) *outCommittedUndoBatchId = batchId;
    auditTrail_.push_back("Committed " + std::to_string(appliedDiffs.size()) +
                          " approved AI action(s) as Undo Batch #" + std::to_string(batchId));
    return true;
}

} // namespace platz::ai
