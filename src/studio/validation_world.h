#pragma once

#include "core/project_state.h"

#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

namespace platz::studio {

struct ValidationStationInfo {
    std::string stationId;
    std::string title;
    std::string gateCoverage;
    std::string capabilityStatus; // "VERIFIED", "INTEGRATED CANDIDATE"
    std::string description;
    std::vector<core::ObjectId> primaryObjectIds;
};

struct ValidationRouteReceipt {
    bool allStationsPassed{false};
    uint32_t stationsVisited{0};
    uint32_t stationsPassed{0};
    core::RevisionId testedRevision{0};
    std::vector<std::string> stationReceipts;
    nlohmann::json ToJson() const;
};

class ValidationWorldManager {
public:
    // Opens or creates the permanent living validation world at projects/PLATZ-Validation
    // and ensures all 6 cumulative Gate 1-3 test stations exist with stable identities (PLZ-TEST-001..008, J-06)
    static bool EnsureValidationWorldPopulated(
        core::ProjectState& project,
        const std::filesystem::path& projectDir,
        std::string* outError = nullptr);

    // Resets a specific test station to its clean baseline without disturbing other stations (PLZ-TEST-008)
    static bool ResetStationToBaseline(
        core::ProjectState& project,
        const std::string& stationId,
        std::string* outError = nullptr);

    // Lists all stations in the active validation world
    static std::vector<ValidationStationInfo> GetStations(const core::ProjectState& project);

    // Executes an automated AI/QA walkthrough route across all stations without corrupting baseline authoring (PLZ-TEST-003)
    static ValidationRouteReceipt RunAutomatedVerificationRoute(core::ProjectState& project);
};

} // namespace platz::studio
