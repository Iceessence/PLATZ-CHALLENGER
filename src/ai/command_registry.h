#pragma once

#include "core/project_state.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

namespace platz::ai {

// Host-enforced CapabilityGrant (PLZ-TRUST-002, PLZ-SAFEX-001, N-19, N-21, N-23)
struct CapabilityGrant {
    std::string granteeId{"PLATZ-AI-Assistant"};
    bool allowReadProject{true};
    bool allowMutateScene{true};
    bool allowMutateMaterials{true};
    bool allowMutateLogic{true};
    bool allowDeleteObjects{false};
    bool allowFileSystemAccess{false};
    bool allowNetworkAccess{false};
    bool allowFinancialOrIdentityUse{false}; // PERMANENTLY FALSE (PLZ-TRUST-003)
    bool allowModelTrainingOnProject{false}; // Opt-in only (PLZ-TRUST-004, N-22)
    bool allowSelfApproval{false};           // PERMANENTLY FALSE (PLZ-OPS-016, N-23)
    uint64_t maxMemoryQuotaBytes{64ULL * 1024ULL * 1024ULL};
    std::filesystem::path confinedProjectRoot;
};

struct TypedCommandRequest {
    std::string commandName; // e.g., "object.set_transform", "object.set_material", "object.create", "object.delete", "object.add_behavior_rule", "project.query_object"
    core::RevisionId expectedRevision{0};
    core::ObjectId targetObjectId{0};
    std::string targetObjectNameHint; // Used only if unambiguous; rejected if multiple objects share name (N-01, N-17)
    nlohmann::json parameters;
    std::string untrustedRetrievedContext; // Inspected as evidence only, never allowed to escalate privileges (N-19)
};

struct CommandValidationResult {
    bool valid{false};
    bool permissionDenied{false};
    bool staleRevision{false};
    bool identityAmbiguousOrMissing{false};
    bool promptInjectionDetected{false};
    std::string errorCode;
    std::string diagnosticMessage;
    nlohmann::json groundedResponse;
};

struct CommandSpec {
    std::string name;
    std::string category;
    std::string description;
    bool mutatesProject{true};
};

class CommandRegistry {
public:
    CommandRegistry();

    const std::vector<CommandSpec>& GetRegisteredCommands() const { return specs_; }
    bool IsKnownCommand(const std::string& commandName) const;

    // Detects prompt-injection / privilege-escalation instructions embedded in retrieved content (PLZ-SAFEX-001, N-19)
    static bool DetectPoisonedRetrievalInjection(
        const std::string& retrievedText,
        std::string* outReason = nullptr);

    // Validates command schema, host capability grant, revision head, and object identity without mutating project
    CommandValidationResult ValidateCommand(
        const core::ProjectState& project,
        const CapabilityGrant& hostGrant,
        const TypedCommandRequest& request) const;

    // Applies a validated command to a mutable target ProjectState (or staging snapshot)
    CommandValidationResult ValidateAndApplyCommand(
        core::ProjectState& targetProject,
        const CapabilityGrant& hostGrant,
        const TypedCommandRequest& request,
        core::ObjectDiffRecord* outDiff = nullptr) const;

    // Sandbox enforcement for untrusted plugins/extensions (PLZ-SAFEX-004, N-21)
    static CommandValidationResult EnforceExtensionSandboxRequest(
        const CapabilityGrant& pluginGrant,
        const std::string& requestedOperation,
        const std::filesystem::path& requestedPath,
        uint64_t requestedMemoryBytes);

private:
    std::vector<CommandSpec> specs_;
};

} // namespace platz::ai
