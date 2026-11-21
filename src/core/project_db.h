#pragma once

#include "core/asset_store.h"
#include "core/object_identity.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace platz::core {

constexpr int32_t kCurrentProjectSchemaVersion = 1;

struct RevisionRecord {
    RevisionId revisionId{0};
    RevisionId parentRevisionId{0};
    std::string branchName{"main"};
    std::string revisionHash;
    std::string author{"PLATZ Studio"};
    std::string summary;
    std::string timestampUtc;
    std::string stateSnapshotJson;
};

struct RuntimeCheckpointRecord {
    std::string slotName{"quicksave_1"};
    RevisionId sourceAuthoringRevision{0};
    double elapsedPlayTimeSec{0.0};
    std::string timestampUtc;
    std::string runtimeStateJson;
};

class ProjectDatabase {
public:
    ProjectDatabase() = default;
    ~ProjectDatabase();

    ProjectDatabase(const ProjectDatabase&) = delete;
    ProjectDatabase& operator=(const ProjectDatabase&) = delete;

    bool OpenOrCreate(
        const std::filesystem::path& dbFilePath,
        const std::string& defaultProjectName,
        std::string* outError = nullptr);

    void Close();
    bool IsOpen() const { return db_ != nullptr; }

    bool GetProjectMetadata(
        std::string* outUuid,
        std::string* outName,
        int32_t* outSchemaVersion,
        RevisionId* outHeadRevision,
        std::string* outError = nullptr) const;

    // Atomic transactional commit of a new revision head, objects table, and assets table (PLZ-CORE-002, PLZ-ARCH-001)
    bool CommitRevisionTransaction(
        const std::string& projectName,
        RevisionId expectedParentRevision,
        const std::string& branchName,
        const std::string& author,
        const std::string& summary,
        const std::vector<SceneObject>& objects,
        const std::vector<AssetMetadata>& assets,
        RevisionId* outNewRevisionId,
        std::string* outError = nullptr,
        bool injectFaultBeforeCommit = false);

    // Load canonical state at head_revision_id (or a specific historical revisionId)
    bool LoadRevisionState(
        std::optional<RevisionId> specificRevision,
        RevisionId* outLoadedRevision,
        std::string* outProjectName,
        std::vector<SceneObject>* outObjects,
        std::vector<AssetMetadata>* outAssets,
        std::string* outError = nullptr) const;

    std::vector<RevisionRecord> ListRevisions() const;

    // Gameplay runtime checkpoints stored separately from editor authoring revisions (PLZ-GAME-016, PLZ-ARCH-006, N-08)
    bool SaveRuntimeCheckpoint(const RuntimeCheckpointRecord& checkpoint, std::string* outError = nullptr);
    std::optional<RuntimeCheckpointRecord> LoadRuntimeCheckpoint(
        const std::string& slotName,
        std::string* outError = nullptr) const;

    // For negative test N-26 (future incompatible schema detection without destructive writes)
    static bool WriteSchemaVersionForTesting(
        const std::filesystem::path& dbFilePath,
        int32_t forcedSchemaVersion,
        std::string* outError = nullptr);

private:
    bool ExecSql(const std::string& sql, std::string* outError = nullptr) const;

    sqlite3* db_{nullptr};
    std::filesystem::path dbPath_;
};

} // namespace platz::core
