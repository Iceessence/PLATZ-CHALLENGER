#include "engine/runtime/runtime_session.h"
#include "platform/platform_paths.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace platz::engine::runtime {

std::vector<LogicDiagnostic> RuntimeSession::ValidateProjectLogic(const core::ProjectState& project) {
    std::vector<LogicDiagnostic> diags;

    for (const auto& obj : project.GetObjects()) {
        std::set<std::pair<std::string, std::string>> stateEdges;
        for (const auto& rule : obj.logic.rules) {
            if ((rule.actionType == "TransferInventory" || rule.triggerType == "OnProximity") &&
                rule.targetObjectId != 0) {
                if (!project.FindObject(rule.targetObjectId)) {
                    diags.push_back({
                        obj.id,
                        obj.name,
                        rule.ruleId,
                        "ERROR",
                        "Rule references nonexistent targetObjectId " + core::FormatObjectId(rule.targetObjectId) + " (N-16)"
                    });
                }
            }
            if (rule.actionType == "TransferInventory" && (rule.targetObjectId == 0 || rule.targetObjectId == obj.id)) {
                diags.push_back({
                    obj.id,
                    obj.name,
                    rule.ruleId,
                    "ERROR",
                    "TransferInventory requires a distinct valid targetObjectId (N-16)"
                });
            }
            if (rule.conditionExpr == "unreachable" || rule.conditionExpr == "false" ||
                (!rule.requiredState.empty() && rule.requiredState == "__UNREACHABLE__")) {
                diags.push_back({
                    obj.id,
                    obj.name,
                    rule.ruleId,
                    "ERROR",
                    "Rule contains unreachable state or always-false condition (N-16)"
                });
            }
            if (rule.actionType == "TransitionState" && !rule.requiredState.empty() && !rule.targetState.empty()) {
                if (rule.requiredState == rule.targetState) {
                    diags.push_back({
                        obj.id,
                        obj.name,
                        rule.ruleId,
                        "WARNING",
                        "Degenerate self-transition state cycle: " + rule.requiredState + " -> " + rule.targetState
                    });
                }
                if (stateEdges.count({rule.targetState, rule.requiredState}) > 0 && rule.triggerType == "OnTick") {
                    diags.push_back({
                        obj.id,
                        obj.name,
                        rule.ruleId,
                        "ERROR",
                        "Unbounded single-tick state machine cycle detected between '" +
                        rule.requiredState + "' and '" + rule.targetState + "' (N-16)"
                    });
                }
                stateEdges.insert({rule.requiredState, rule.targetState});
            }
        }
    }
    return diags;
}

bool RuntimeSession::StartPlay(const core::ProjectState& canonicalProject, std::string* outError) {
    StopPlay();

    runtimeObjects_ = canonicalProject.GetObjects();
    sourceAuthoringRevision_ = canonicalProject.GetHeadRevision();
    metrics_ = {};
    eventTraceLog_.clear();
    debuggerTimeline_.clear();
    playing_ = true;
    paused_ = false;

    AppendTrace("Started isolated Play session from authoring revision r" + std::to_string(sourceAuthoringRevision_));

    for (auto& obj : runtimeObjects_) {
        obj.transform.Sanitize();
        for (const auto& rule : obj.logic.rules) {
            if (rule.triggerType == "OnStart") {
                ++metrics_.rulesEvaluatedCount;
                if (EvaluateRuleCondition(obj, rule)) {
                    std::string beforeState = obj.logic.currentState;
                    ExecuteRuleAction(obj, rule, 0.0);
                    RecordDebuggerTimelineEntry(obj, rule, beforeState);
                    ++metrics_.rulesTriggeredCount;
                }
            }
        }
    }
    if (outError) outError->clear();
    return true;
}

void RuntimeSession::StopPlay() {
    playing_ = false;
    paused_ = false;
    runtimeObjects_.clear();
}

bool RuntimeSession::ApplyRuntimeObjectToAuthoring(
    core::ObjectId objectId,
    core::ProjectState& canonicalProject,
    std::string* outError) const {
    if (!playing_) {
        if (outError) *outError = "Runtime session is not active";
        return false;
    }
    const core::SceneObject* rtObj = FindRuntimeObject(objectId);
    if (!rtObj) {
        if (outError) *outError = "Object not found in runtime projection";
        return false;
    }
    return canonicalProject.ApplyObjectMutation(
        objectId,
        *rtObj,
        "Apply Play State for " + rtObj->name,
        "RuntimeApplyBack",
        "User");
}

core::SceneObject* RuntimeSession::FindRuntimeObject(core::ObjectId id) {
    for (auto& o : runtimeObjects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

const core::SceneObject* RuntimeSession::FindRuntimeObject(core::ObjectId id) const {
    for (const auto& o : runtimeObjects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

void RuntimeSession::AppendTrace(const std::string& msg) {
    if (eventTraceLog_.size() >= 120) {
        eventTraceLog_.erase(eventTraceLog_.begin());
    }
    eventTraceLog_.push_back(msg);
}

void RuntimeSession::RecordDebuggerTimelineEntry(
    const core::SceneObject& self,
    const core::BehaviorRule& rule,
    const std::string& stateBefore) {
    if (debuggerTimeline_.size() >= 256) {
        debuggerTimeline_.erase(debuggerTimeline_.begin());
    }
    RuleDebuggerTimelineEntry entry;
    entry.stepIndex = metrics_.stepCount;
    entry.timestampSec = metrics_.elapsedPlayTimeSec;
    entry.objectId = self.id;
    entry.objectName = self.name;
    entry.ruleId = rule.ruleId;
    entry.triggerType = rule.triggerType;
    entry.actionType = rule.actionType;
    entry.stateBefore = stateBefore;
    entry.stateAfter = self.logic.currentState;
    entry.positionAfter = self.transform.position;
    auto it = self.logic.inventory.find("energy_crystal");
    entry.energyCrystalsAfter = (it != self.logic.inventory.end()) ? it->second : 0;
    debuggerTimeline_.push_back(std::move(entry));
}

bool RuntimeSession::EvaluateRuleCondition(const core::SceneObject& self, const core::BehaviorRule& rule) const {
    if (rule.conditionExpr == "false" || rule.conditionExpr == "unreachable") {
        return false;
    }
    if (!rule.requiredState.empty() && self.logic.currentState != rule.requiredState) {
        return false;
    }
    if (rule.triggerType == "OnProximity" && rule.targetObjectId != 0) {
        const core::SceneObject* target = FindRuntimeObject(rule.targetObjectId);
        if (!target) return false;
        float dist = glm::length(target->transform.position - self.transform.position);
        float radius = (rule.paramA > 0.01f) ? rule.paramA : 4.0f;
        if (dist > radius) return false;
    }
    if (rule.conditionExpr == "inventory >= amount") {
        auto it = self.logic.inventory.find(rule.resourceName);
        int64_t avail = (it != self.logic.inventory.end()) ? it->second : 0;
        if (avail < rule.resourceAmount) return false;
    }
    return true;
}

void RuntimeSession::ExecuteRuleAction(core::SceneObject& self, const core::BehaviorRule& rule, double dt) {
    float fdt = static_cast<float>(dt);
    if (rule.actionType == "RotateY") {
        self.transform.rotationEulerDeg.y += rule.paramA * fdt;
        if (self.transform.rotationEulerDeg.y >= 360.0f) self.transform.rotationEulerDeg.y -= 360.0f;
        if (self.transform.rotationEulerDeg.y < 0.0f) self.transform.rotationEulerDeg.y += 360.0f;
    } else if (rule.actionType == "BobVertical") {
        float amp = (std::abs(rule.paramA) > 1e-4f) ? rule.paramA : 0.35f;
        float freq = (std::abs(rule.paramB) > 1e-4f) ? rule.paramB : 2.0f;
        float baseY = 1.0f;
        auto it = self.logic.variables.find("base_y");
        if (it != self.logic.variables.end()) {
            baseY = static_cast<float>(it->second);
        } else {
            self.logic.variables["base_y"] = self.transform.position.y;
            baseY = self.transform.position.y;
        }
        self.transform.position.y = baseY + std::sin(static_cast<float>(metrics_.elapsedPlayTimeSec) * freq) * amp;
    } else if (rule.actionType == "OrbitOrigin") {
        float radius = (rule.paramA > 0.1f) ? rule.paramA : 4.0f;
        float angularSpeed = (std::abs(rule.paramB) > 1e-4f) ? rule.paramB : 0.8f;
        float angle = static_cast<float>(metrics_.elapsedPlayTimeSec) * angularSpeed;
        self.transform.position.x = std::cos(angle) * radius;
        self.transform.position.z = std::sin(angle) * radius;
    } else if (rule.actionType == "PatrolWaypoints") {
        // Patrol back and forth along X/Z span around origin_x
        float span = (std::abs(rule.paramA) > 0.1f) ? rule.paramA : 2.5f;
        float speed = (std::abs(rule.paramB) > 0.05f) ? rule.paramB : 1.4f;
        float origX = self.transform.position.x;
        auto it = self.logic.variables.find("origin_x");
        if (it != self.logic.variables.end()) {
            origX = static_cast<float>(it->second);
        } else {
            self.logic.variables["origin_x"] = origX;
        }
        self.transform.position.x = origX + std::sin(static_cast<float>(metrics_.elapsedPlayTimeSec) * speed) * span;
        self.logic.variables["patrol_phase"] = std::sin(static_cast<float>(metrics_.elapsedPlayTimeSec) * speed);
    } else if (rule.actionType == "ModifyVariable") {
        std::string varName = rule.resourceName.empty() ? "score" : rule.resourceName;
        self.logic.variables[varName] += static_cast<double>(rule.paramA) * dt;
    } else if (rule.actionType == "TransitionState") {
        if (!rule.targetState.empty() && self.logic.currentState != rule.targetState) {
            std::string oldState = self.logic.currentState;
            self.logic.currentState = rule.targetState;
            AppendTrace(self.name + " state: " + oldState + " -> " + self.logic.currentState);
        }
    } else if (rule.actionType == "PulseEmissive") {
        float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(metrics_.elapsedPlayTimeSec) * rule.paramB);
        self.material.emissiveStrength = std::max(0.0f, rule.paramA * pulse);
    } else if (rule.actionType == "TransferInventory") {
        ExecuteConservedResourceTransfer(
            self.id,
            rule.targetObjectId,
            rule.resourceName,
            rule.resourceAmount,
            false,
            nullptr);
    }
    self.transform.Sanitize();
}

void RuntimeSession::StepSimulation(double deltaSeconds) {
    if (!playing_ || paused_) return;
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0) return;

    double clampedDt = std::min(deltaSeconds, 0.1);
    constexpr double kFixedSubstep = 1.0 / 120.0;
    double remaining = clampedDt;

    while (remaining > 1e-6) {
        double dt = std::min(remaining, kFixedSubstep);
        remaining -= dt;
        float fdt = static_cast<float>(dt);

        for (auto& obj : runtimeObjects_) {
            if (!obj.physics.enabled || !obj.physics.isDynamic) continue;

            if (!std::isfinite(obj.physics.linearVelocity.x) ||
                !std::isfinite(obj.physics.linearVelocity.y) ||
                !std::isfinite(obj.physics.linearVelocity.z)) {
                obj.physics.linearVelocity = glm::vec3(0.0f);
            }
            float maxSpeed = 120.0f;
            float speed = glm::length(obj.physics.linearVelocity);
            if (speed > maxSpeed) {
                obj.physics.linearVelocity = (obj.physics.linearVelocity / speed) * maxSpeed;
            }

            obj.physics.linearVelocity.y -= 9.81f * fdt;
            obj.transform.position += obj.physics.linearVelocity * fdt;
            obj.transform.rotationEulerDeg += obj.physics.angularVelocityDeg * fdt;

            float floorY = std::max(0.0f, obj.physics.colliderHalfExtents.y * std::abs(obj.transform.scale.y));
            if (obj.transform.position.y < floorY) {
                obj.transform.position.y = floorY;
                if (obj.physics.linearVelocity.y < 0.0f) {
                    obj.physics.linearVelocity.y = -obj.physics.linearVelocity.y * std::clamp(obj.physics.restitution, 0.0f, 1.0f);
                    if (std::abs(obj.physics.linearVelocity.y) < 0.15f) {
                        obj.physics.linearVelocity.y = 0.0f;
                    }
                    float damp = std::clamp(1.0f - obj.physics.friction * fdt * 4.0f, 0.0f, 1.0f);
                    obj.physics.linearVelocity.x *= damp;
                    obj.physics.linearVelocity.z *= damp;
                    ++metrics_.collisionsResolvedCount;
                }
            }
            obj.transform.Sanitize();
        }
    }

    metrics_.elapsedPlayTimeSec += clampedDt;
    ++metrics_.stepCount;

    for (auto& obj : runtimeObjects_) {
        for (const auto& rule : obj.logic.rules) {
            if (rule.triggerType == "OnTick" || rule.triggerType == "OnProximity") {
                ++metrics_.rulesEvaluatedCount;
                if (EvaluateRuleCondition(obj, rule)) {
                    std::string beforeState = obj.logic.currentState;
                    ExecuteRuleAction(obj, rule, clampedDt);
                    RecordDebuggerTimelineEntry(obj, rule, beforeState);
                    ++metrics_.rulesTriggeredCount;
                }
            }
        }
    }
}

bool RuntimeSession::TriggerInteraction(core::ObjectId objectId, std::string* outEventLog) {
    if (!playing_) return false;
    core::SceneObject* obj = FindRuntimeObject(objectId);
    if (!obj) return false;

    bool anyTriggered = false;
    for (const auto& rule : obj->logic.rules) {
        if (rule.triggerType == "OnInteract") {
            ++metrics_.rulesEvaluatedCount;
            if (EvaluateRuleCondition(*obj, rule)) {
                std::string beforeState = obj->logic.currentState;
                ExecuteRuleAction(*obj, rule, 0.016);
                RecordDebuggerTimelineEntry(*obj, rule, beforeState);
                ++metrics_.rulesTriggeredCount;
                anyTriggered = true;
            }
        }
    }
    std::string msg = "Interaction triggered on " + obj->name + " (" + core::FormatObjectId(objectId) +
                      ") -> state=" + obj->logic.currentState;
    AppendTrace(msg);
    if (outEventLog) *outEventLog = msg;
    return anyTriggered;
}

bool RuntimeSession::ExecuteConservedResourceTransfer(
    core::ObjectId fromObjectId,
    core::ObjectId toObjectId,
    const std::string& resourceName,
    int64_t amount,
    bool simulateInterruptionMidTransfer,
    std::string* outError) {
    if (!playing_) {
        if (outError) *outError = "Runtime session is not playing";
        return false;
    }
    if (amount <= 0 || fromObjectId == toObjectId) {
        if (outError) *outError = "Invalid transfer amount or identical source/target object";
        return false;
    }
    core::SceneObject* src = FindRuntimeObject(fromObjectId);
    core::SceneObject* dst = FindRuntimeObject(toObjectId);
    if (!src || !dst) {
        if (outError) *outError = "Source or destination object missing";
        return false;
    }

    int64_t srcBefore = src->logic.inventory[resourceName];
    int64_t dstBefore = dst->logic.inventory[resourceName];
    if (srcBefore < amount) {
        if (outError) *outError = "Insufficient conserved inventory on source object";
        return false;
    }

    src->logic.inventory[resourceName] = srcBefore - amount;

    if (simulateInterruptionMidTransfer) {
        src->logic.inventory[resourceName] = srcBefore;
        dst->logic.inventory[resourceName] = dstBefore;
        ++metrics_.conservedTransfersRolledBack;
        if (outError) *outError = "Transfer interrupted mid-operation; transaction rolled back cleanly (N-09)";
        return false;
    }

    dst->logic.inventory[resourceName] = dstBefore + amount;
    ++metrics_.conservedTransfersSucceeded;
    AppendTrace("Transferred " + std::to_string(amount) + " " + resourceName +
                " from " + src->name + " to " + dst->name);
    return true;
}

int64_t RuntimeSession::ComputeTotalResourceAcrossWorld(const std::string& resourceName) const {
    int64_t total = 0;
    for (const auto& obj : runtimeObjects_) {
        auto it = obj.logic.inventory.find(resourceName);
        if (it != obj.logic.inventory.end()) {
            total += it->second;
        }
    }
    return total;
}

bool RuntimeSession::SaveGameplayCheckpoint(
    core::ProjectState& projectForDb,
    const std::string& slotName,
    std::string* outError) const {
    if (!playing_) {
        if (outError) *outError = "Cannot save gameplay checkpoint when not in Play mode";
        return false;
    }

    nlohmann::json state;
    state["slotName"] = slotName;
    state["sourceAuthoringRevision"] = sourceAuthoringRevision_;
    state["elapsedPlayTimeSec"] = metrics_.elapsedPlayTimeSec;
    nlohmann::json objsArr = nlohmann::json::array();
    for (const auto& o : runtimeObjects_) {
        objsArr.push_back(o.ToJson());
    }
    state["runtimeObjects"] = std::move(objsArr);

    core::RuntimeCheckpointRecord rec;
    rec.slotName = slotName;
    rec.sourceAuthoringRevision = sourceAuthoringRevision_;
    rec.elapsedPlayTimeSec = metrics_.elapsedPlayTimeSec;
    rec.timestampUtc = platform::UtcIso8601Now();
    rec.runtimeStateJson = state.dump();

    return projectForDb.GetDatabase().SaveRuntimeCheckpoint(rec, outError);
}

bool RuntimeSession::LoadGameplayCheckpoint(
    const core::ProjectState& projectForDb,
    const std::string& slotName,
    std::string* outError) {
    auto recOpt = projectForDb.GetDatabase().LoadRuntimeCheckpoint(slotName, outError);
    if (!recOpt) {
        return false;
    }

    auto j = nlohmann::json::parse(recOpt->runtimeStateJson, nullptr, false);
    if (j.is_discarded() || !j.contains("runtimeObjects") || !j["runtimeObjects"].is_array()) {
        if (outError) *outError = "Corrupt runtime checkpoint JSON";
        return false;
    }

    std::vector<core::SceneObject> loaded;
    for (const auto& oj : j["runtimeObjects"]) {
        loaded.push_back(core::SceneObject::FromJson(oj));
    }

    runtimeObjects_ = std::move(loaded);
    sourceAuthoringRevision_ = recOpt->sourceAuthoringRevision;
    metrics_.elapsedPlayTimeSec = recOpt->elapsedPlayTimeSec;
    playing_ = true;
    paused_ = false;
    AppendTrace("Resumed gameplay checkpoint '" + slotName + "' at t=" +
                std::to_string(metrics_.elapsedPlayTimeSec) + "s");
    return true;
}

} // namespace platz::engine::runtime
