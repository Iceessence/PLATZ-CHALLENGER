#include "ai/command_registry.h"
#include "platform/platform_paths.h"

#include <algorithm>
#include <cctype>

namespace platz::ai {

namespace {

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

CommandRegistry::CommandRegistry() {
    specs_ = {
        {"project.query_object", "Query", "Inspect grounded canonical object properties and revision citation", false},
        {"object.create", "Scene", "Create a new canonical 3D SceneObject with validated transform and mesh", true},
        {"object.set_transform", "Scene", "Update position, rotationEulerDeg, or scale of a stable ObjectId", true},
        {"object.set_material", "Material", "Update PBR metallic, roughness, baseColor, or emissive parameters", true},
        {"object.set_modifier_stack", "3D Modeling", "Update non-destructive 3D mesh modifier stack on a stable ObjectId", true},
        {"object.set_material_graph", "Material", "Attach a validated Visual Material Node Graph JSON to a stable ObjectId", true},
        {"object.add_behavior_rule", "Logic", "Attach a validated no-code BehaviorRule to a stable ObjectId", true},
        {"object.delete", "Scene", "Delete a canonical SceneObject by stable ObjectId (requires allowDeleteObjects)", true}
    };
}

bool CommandRegistry::IsKnownCommand(const std::string& commandName) const {
    for (const auto& s : specs_) {
        if (s.name == commandName) return true;
    }
    return false;
}

bool CommandRegistry::DetectPoisonedRetrievalInjection(
    const std::string& retrievedText,
    std::string* outReason) {
    if (retrievedText.empty()) return false;
    std::string lower = ToLowerAscii(retrievedText);

    const char* signatures[] = {
        "ignore previous instructions",
        "escalate privilege",
        "grant allow",
        "bypass permission",
        "execute shell",
        "cmd.exe",
        "powershell",
        "self_approve",
        "independently_accepted",
        "transfer_money",
        "c:\\platz\\",
        "c:/platz/"
    };
    for (const char* sig : signatures) {
        if (lower.find(sig) != std::string::npos) {
            if (outReason) {
                *outReason = std::string("Poisoned retrieval prompt-injection signature blocked (PLZ-SAFEX-001 / N-19): '") + sig + "'";
            }
            return true;
        }
    }
    return false;
}

CommandValidationResult CommandRegistry::ValidateCommand(
    const core::ProjectState& project,
    const CapabilityGrant& hostGrant,
    const TypedCommandRequest& request) const {
    CommandValidationResult res{};

    std::string injectionReason;
    if (DetectPoisonedRetrievalInjection(request.untrustedRetrievedContext, &injectionReason)) {
        res.permissionDenied = true;
        res.promptInjectionDetected = true;
        res.errorCode = "ERR_POISONED_RETRIEVAL_BLOCKED";
        res.diagnosticMessage = injectionReason;
        return res;
    }

    if (request.commandName == "model.self_approve" ||
        request.commandName == "release.promote_independently_accepted" ||
        request.commandName == "account.payment") {
        res.permissionDenied = true;
        res.errorCode = "ERR_FORBIDDEN_AUTHORITY_ESCALATION";
        res.diagnosticMessage = "Self-approval and financial/identity operations are strictly forbidden by host constitution (N-23)";
        return res;
    }

    if (!IsKnownCommand(request.commandName)) {
        res.errorCode = "ERR_UNKNOWN_COMMAND";
        res.diagnosticMessage = "Command '" + request.commandName + "' is not in the versioned CommandRegistry (PLZ-SAFEX-007)";
        return res;
    }

    if (request.commandName == "project.query_object" && !hostGrant.allowReadProject) {
        res.permissionDenied = true;
        res.errorCode = "ERR_PERMISSION_DENIED";
        res.diagnosticMessage = "Host CapabilityGrant denies allowReadProject";
        return res;
    }
    if ((request.commandName == "object.create" ||
         request.commandName == "object.set_transform" ||
         request.commandName == "object.set_modifier_stack") &&
        !hostGrant.allowMutateScene) {
        res.permissionDenied = true;
        res.errorCode = "ERR_PERMISSION_DENIED";
        res.diagnosticMessage = "Host CapabilityGrant denies allowMutateScene";
        return res;
    }
    if ((request.commandName == "object.set_material" ||
         request.commandName == "object.set_material_graph") &&
        !hostGrant.allowMutateMaterials) {
        res.permissionDenied = true;
        res.errorCode = "ERR_PERMISSION_DENIED";
        res.diagnosticMessage = "Host CapabilityGrant denies allowMutateMaterials";
        return res;
    }
    if (request.commandName == "object.add_behavior_rule" && !hostGrant.allowMutateLogic) {
        res.permissionDenied = true;
        res.errorCode = "ERR_PERMISSION_DENIED";
        res.diagnosticMessage = "Host CapabilityGrant denies allowMutateLogic";
        return res;
    }
    if (request.commandName == "object.delete" && !hostGrant.allowDeleteObjects) {
        res.permissionDenied = true;
        res.errorCode = "ERR_PERMISSION_DENIED";
        res.diagnosticMessage = "Host CapabilityGrant denies allowDeleteObjects";
        return res;
    }

    if (request.expectedRevision != project.GetHeadRevision()) {
        res.staleRevision = true;
        res.errorCode = "ERR_STALE_REVISION";
        res.diagnosticMessage = "Stale revision rejected (N-02): command expected revision r" +
                                std::to_string(request.expectedRevision) + ", but current head is r" +
                                std::to_string(project.GetHeadRevision());
        return res;
    }

    if (request.commandName != "object.create") {
        core::ObjectId resolvedId = request.targetObjectId;
        if (resolvedId == 0 && !request.targetObjectNameHint.empty()) {
            auto matches = project.FindObjectsByName(request.targetObjectNameHint);
            if (matches.size() > 1) {
                res.identityAmbiguousOrMissing = true;
                res.errorCode = "ERR_AMBIGUOUS_OBJECT_NAME";
                res.diagnosticMessage = "Ambiguous object name '" + request.targetObjectNameHint +
                                        "' matches " + std::to_string(matches.size()) +
                                        " distinct ObjectIds; explicit stable ObjectId required (N-01 / N-17)";
                return res;
            } else if (matches.size() == 1) {
                resolvedId = matches[0];
            }
        }

        const core::SceneObject* targetObj = project.FindObject(resolvedId);
        if (!targetObj) {
            res.identityAmbiguousOrMissing = true;
            res.errorCode = "ERR_TARGET_OBJECT_NOT_FOUND";
            res.diagnosticMessage = "Target ObjectId " + core::FormatObjectId(resolvedId) +
                                    " does not exist in canonical project state (N-17)";
            return res;
        }
        if (targetObj->locked && request.commandName != "project.query_object") {
            res.permissionDenied = true;
            res.errorCode = "ERR_OBJECT_LOCKED";
            res.diagnosticMessage = "Target object '" + targetObj->name + "' is locked";
            return res;
        }
    }

    const auto& p = request.parameters;
    if (!p.is_object()) {
        res.errorCode = "ERR_INVALID_SCHEMA";
        res.diagnosticMessage = "Command parameters must be a JSON object";
        return res;
    }

    if (request.commandName == "object.set_transform") {
        if (!p.contains("position") && !p.contains("rotationEulerDeg") && !p.contains("scale")) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "object.set_transform requires at least one of position, rotationEulerDeg, or scale";
            return res;
        }
        auto checkVec3 = [&](const char* key, bool nonZero) -> bool {
            if (!p.contains(key)) return true;
            if (!p[key].is_array() || p[key].size() != 3) return false;
            for (int i = 0; i < 3; ++i) {
                if (!p[key][i].is_number()) return false;
                float v = p[key][i].get<float>();
                if (!std::isfinite(v)) return false;
                if (nonZero && std::abs(v) < 1e-4f) return false;
            }
            return true;
        };
        if (!checkVec3("position", false) || !checkVec3("rotationEulerDeg", false) || !checkVec3("scale", true)) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "Invalid or non-finite transform vector in object.set_transform (N-10)";
            return res;
        }
    } else if (request.commandName == "object.set_material") {
        if (p.contains("metallic") && (!p["metallic"].is_number() || p["metallic"].get<float>() < 0.0f || p["metallic"].get<float>() > 1.0f)) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "metallic must be a number in [0.0, 1.0]";
            return res;
        }
        if (p.contains("roughness") && (!p["roughness"].is_number() || p["roughness"].get<float>() < 0.04f || p["roughness"].get<float>() > 1.0f)) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "roughness must be a number in [0.04, 1.0]";
            return res;
        }
    } else if (request.commandName == "object.set_modifier_stack") {
        if (!p.contains("modifiers") || !p["modifiers"].is_array()) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "object.set_modifier_stack requires a 'modifiers' array";
            return res;
        }
    } else if (request.commandName == "object.set_material_graph") {
        if (!p.contains("materialGraphJson") || !p["materialGraphJson"].is_string()) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "object.set_material_graph requires string 'materialGraphJson'";
            return res;
        }
    } else if (request.commandName == "object.create") {
        if (!p.contains("name") || !p["name"].is_string() || p["name"].get<std::string>().empty()) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "object.create requires a non-empty string 'name'";
            return res;
        }
    } else if (request.commandName == "object.add_behavior_rule") {
        if (!p.contains("actionType") || !p["actionType"].is_string()) {
            res.errorCode = "ERR_INVALID_SCHEMA";
            res.diagnosticMessage = "object.add_behavior_rule requires string 'actionType'";
            return res;
        }
    }

    res.valid = true;
    res.errorCode = "OK";
    res.diagnosticMessage = "Command validated at revision r" + std::to_string(project.GetHeadRevision());
    return res;
}

CommandValidationResult CommandRegistry::ValidateAndApplyCommand(
    core::ProjectState& targetProject,
    const CapabilityGrant& hostGrant,
    const TypedCommandRequest& request,
    core::ObjectDiffRecord* outDiff) const {
    CommandValidationResult res = ValidateCommand(targetProject, hostGrant, request);
    if (!res.valid) {
        return res;
    }

    core::ObjectId resolvedId = request.targetObjectId;
    if (resolvedId == 0 && !request.targetObjectNameHint.empty() && request.commandName != "object.create") {
        auto matches = targetProject.FindObjectsByName(request.targetObjectNameHint);
        if (matches.size() == 1) resolvedId = matches[0];
    }

    const auto& p = request.parameters;

    if (request.commandName == "project.query_object") {
        const core::SceneObject* obj = targetProject.FindObject(resolvedId);
        res.groundedResponse = {
            {"projectName", targetProject.GetProjectName()},
            {"headRevision", targetProject.GetHeadRevision()},
            {"objectId", obj->id},
            {"formattedObjectId", core::FormatObjectId(obj->id)},
            {"object", obj->ToJson()},
            {"citation", targetProject.GetProjectName() + "@r" + std::to_string(targetProject.GetHeadRevision()) + ":" + core::FormatObjectId(obj->id)}
        };
        return res;
    }

    if (request.commandName == "object.create") {
        std::string name = p.value("name", std::string("AI_Created_Object"));
        std::string prim = p.value("primitiveType", std::string("cube"));
        auto& created = targetProject.CreateObject(name, prim, 0, "AI", hostGrant.granteeId);
        if (p.contains("position") && p["position"].is_array() && p["position"].size() == 3) {
            created.transform.position = {p["position"][0].get<float>(), p["position"][1].get<float>(), p["position"][2].get<float>()};
        }
        if (p.contains("scale") && p["scale"].is_array() && p["scale"].size() == 3) {
            created.transform.scale = {p["scale"][0].get<float>(), p["scale"][1].get<float>(), p["scale"][2].get<float>()};
        }
        if (p.contains("baseColor") && p["baseColor"].is_array() && p["baseColor"].size() >= 3) {
            created.material.baseColor = {
                p["baseColor"][0].get<float>(),
                p["baseColor"][1].get<float>(),
                p["baseColor"][2].get<float>(),
                p["baseColor"].size() >= 4 ? p["baseColor"][3].get<float>() : 1.0f
            };
        }
        created.transform.Sanitize();

        if (outDiff) {
            outDiff->objectId = created.id;
            outDiff->existedBefore = false;
            outDiff->existsAfter = true;
            outDiff->afterJson = created.ToJson();
        }
        res.groundedResponse = {{"createdObjectId", created.id}};
        return res;
    }

    if (request.commandName == "object.delete") {
        const core::SceneObject* existing = targetProject.FindObject(resolvedId);
        if (outDiff && existing) {
            outDiff->objectId = resolvedId;
            outDiff->existedBefore = true;
            outDiff->existsAfter = false;
            outDiff->beforeJson = existing->ToJson();
        }
        targetProject.DeleteObject(resolvedId, hostGrant.granteeId);
        return res;
    }

    core::SceneObject* existing = targetProject.FindObject(resolvedId);
    core::SceneObject updated = *existing;
    nlohmann::json beforeJ = existing->ToJson();

    if (request.commandName == "object.set_transform") {
        if (p.contains("position")) {
            updated.transform.position = {p["position"][0].get<float>(), p["position"][1].get<float>(), p["position"][2].get<float>()};
        }
        if (p.contains("rotationEulerDeg")) {
            updated.transform.rotationEulerDeg = {p["rotationEulerDeg"][0].get<float>(), p["rotationEulerDeg"][1].get<float>(), p["rotationEulerDeg"][2].get<float>()};
        }
        if (p.contains("scale")) {
            updated.transform.scale = {p["scale"][0].get<float>(), p["scale"][1].get<float>(), p["scale"][2].get<float>()};
        }
        updated.transform.Sanitize();
    } else if (request.commandName == "object.set_material") {
        if (p.contains("materialName") && p["materialName"].is_string()) {
            updated.material.materialName = p["materialName"].get<std::string>();
        }
        if (p.contains("baseColor") && p["baseColor"].is_array() && p["baseColor"].size() >= 3) {
            updated.material.baseColor = {
                p["baseColor"][0].get<float>(),
                p["baseColor"][1].get<float>(),
                p["baseColor"][2].get<float>(),
                p["baseColor"].size() >= 4 ? p["baseColor"][3].get<float>() : 1.0f
            };
        }
        if (p.contains("metallic")) updated.material.metallic = p["metallic"].get<float>();
        if (p.contains("roughness")) updated.material.roughness = p["roughness"].get<float>();
        if (p.contains("emissiveStrength")) updated.material.emissiveStrength = p["emissiveStrength"].get<float>();
        if (p.contains("emissiveColor") && p["emissiveColor"].is_array() && p["emissiveColor"].size() == 3) {
            updated.material.emissiveColor = {
                p["emissiveColor"][0].get<float>(),
                p["emissiveColor"][1].get<float>(),
                p["emissiveColor"][2].get<float>()
            };
        }
        if (p.contains("useProceduralChecker") && p["useProceduralChecker"].is_boolean()) {
            updated.material.useProceduralChecker = p["useProceduralChecker"].get<bool>();
        }
    } else if (request.commandName == "object.set_modifier_stack") {
        updated.mesh.modifierStack.clear();
        for (const auto& mj : p["modifiers"]) {
            core::MeshModifier mod;
            mod.modifierId = mj.value("modifierId", "ai_mod_" + std::to_string(updated.mesh.modifierStack.size() + 1));
            mod.modifierType = mj.value("modifierType", std::string("Subdivide"));
            mod.enabled = mj.value("enabled", true);
            mod.paramA = mj.value("paramA", 0.25f);
            mod.paramB = mj.value("paramB", 1.0f);
            mod.seed = mj.value("seed", 2026u);
            updated.mesh.modifierStack.push_back(std::move(mod));
        }
    } else if (request.commandName == "object.set_material_graph") {
        updated.material.materialGraphJson = p["materialGraphJson"].get<std::string>();
    } else if (request.commandName == "object.add_behavior_rule") {
        core::BehaviorRule rule;
        rule.ruleId = p.value("ruleId", "ai_rule_" + std::to_string(updated.logic.rules.size() + 1));
        rule.triggerType = p.value("triggerType", std::string("OnTick"));
        rule.conditionExpr = p.value("conditionExpr", std::string("always"));
        rule.actionType = p.value("actionType", std::string("RotateY"));
        rule.paramA = p.value("paramA", 45.0f);
        rule.paramB = p.value("paramB", 1.5f);
        rule.requiredState = p.value("requiredState", std::string(""));
        rule.targetState = p.value("targetState", std::string(""));
        rule.resourceName = p.value("resourceName", std::string("energy_crystal"));
        rule.resourceAmount = p.value("resourceAmount", int64_t{1});
        rule.targetObjectId = p.value("targetObjectId", core::ObjectId{0});
        updated.logic.rules.push_back(std::move(rule));
    }

    targetProject.ApplyObjectMutation(resolvedId, updated, "AI: " + request.commandName, "AI", hostGrant.granteeId);
    if (outDiff) {
        outDiff->objectId = resolvedId;
        outDiff->existedBefore = true;
        outDiff->existsAfter = true;
        outDiff->beforeJson = std::move(beforeJ);
        outDiff->afterJson = updated.ToJson();
    }
    return res;
}

CommandValidationResult CommandRegistry::EnforceExtensionSandboxRequest(
    const CapabilityGrant& pluginGrant,
    const std::string& requestedOperation,
    const std::filesystem::path& requestedPath,
    uint64_t requestedMemoryBytes) {
    CommandValidationResult res{};

    if (requestedMemoryBytes > pluginGrant.maxMemoryQuotaBytes) {
        res.permissionDenied = true;
        res.errorCode = "ERR_QUOTA_EXCEEDED";
        res.diagnosticMessage = "Extension memory request (" + std::to_string(requestedMemoryBytes) +
                                " B) exceeds enforced sandbox quota (" +
                                std::to_string(pluginGrant.maxMemoryQuotaBytes) + " B) (PLZ-SAFEX-004 / N-21)";
        return res;
    }

    if (requestedOperation == "network.socket" && !pluginGrant.allowNetworkAccess) {
        res.permissionDenied = true;
        res.errorCode = "ERR_NETWORK_DENIED";
        res.diagnosticMessage = "Untrusted extension denied network socket access (PLZ-SAFEX-004 / N-21)";
        return res;
    }

    if (requestedOperation == "fs.read" || requestedOperation == "fs.write") {
        if (!pluginGrant.allowFileSystemAccess) {
            res.permissionDenied = true;
            res.errorCode = "ERR_FILESYSTEM_DENIED";
            res.diagnosticMessage = "Untrusted extension denied host filesystem access (N-21)";
            return res;
        }
        auto pathCheck = platform::ResolveConfinedPath(pluginGrant.confinedProjectRoot, requestedPath);
        if (!pathCheck.allowed) {
            res.permissionDenied = true;
            res.errorCode = "ERR_PATH_CONFINEMENT_VIOLATION";
            res.diagnosticMessage = pathCheck.violationReason;
            return res;
        }
    }

    res.valid = true;
    res.errorCode = "OK";
    return res;
}

} // namespace platz::ai
