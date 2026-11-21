#pragma once

#include "core/asset_store.h"
#include "core/object_identity.h"
#include "core/project_db.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace platz::core {

struct ObjectDiffRecord {
    ObjectId objectId{0};
    bool existedBefore{false};
    bool existsAfter{false};
    nlohmann::json beforeJson;
    nlohmann::json afterJson;
};

struct UndoBatchEntry {
    uint64_t batchId{0};
    std::string label;
    std::string domain{"Scene"}; // "Scene", "Material", "Logic", "Asset", "AI"
    std::string author{"User"};
    std::vector<ObjectDiffRecord> objectDiffs;
};

struct ExportProvenanceReport {
    bool allowed{false};
    std::filesystem::path outputBundlePath;
    RevisionId sourceRevision{0};
    size_t objectCount{0};
    size_t assetCount{0};
    std::vector<std::string> unlicensedAssets;
    std::string errorMessage;
};

class ProjectState {
public:
    ProjectState();

    bool CreateOrOpenProject(
        const std::filesystem::path& projectDir,
        const std::string& defaultName = "PLATZ-Project",
        std::string* outError = nullptr);

    bool SaveProject(
        const std::string& summary = "Save project state",
        const std::string& author = "PLATZ Studio",
        std::string* outError = nullptr,
        bool injectFaultBeforeCommit = false);

    bool ReloadFromDisk(std::optional<RevisionId> specificRevision = std::nullopt, std::string* outError = nullptr);

    const std::filesystem::path& GetProjectDir() const { return projectDir_; }
    const std::string& GetProjectName() const { return projectName_; }
    void SetProjectName(const std::string& name) { projectName_ = name; dirty_ = true; }
    RevisionId GetHeadRevision() const { return headRevision_; }
    const std::string& GetActiveBranch() const { return activeBranch_; }
    void SetActiveBranch(const std::string& branch) { activeBranch_ = branch; }
    bool IsDirty() const { return dirty_; }

    // Canonical object operations (all record scoped undo batches automatically unless recordUndo=false)
    const std::vector<SceneObject>& GetObjects() const { return objects_; }
    std::vector<SceneObject>& GetObjectsMutable() { dirty_ = true; return objects_; }

    SceneObject* FindObject(ObjectId id);
    const SceneObject* FindObject(ObjectId id) const;
    std::vector<ObjectId> FindObjectsByName(std::string_view name) const;

    SceneObject& CreateObject(
        const std::string& name,
        const std::string& primitiveType = "cube",
        ObjectId parentId = 0,
        const std::string& domain = "Scene",
        const std::string& author = "User");

    std::optional<ObjectId> DuplicateObject(ObjectId sourceId, const std::string& author = "User");
    bool DeleteObject(ObjectId id, const std::string& author = "User");

    bool ApplyObjectMutation(
        ObjectId id,
        const SceneObject& updated,
        const std::string& actionLabel,
        const std::string& domain = "Scene",
        const std::string& author = "User",
        uint64_t* outBatchId = nullptr);

    // Hierarchy world transform (PLZ-GAME-001)
    glm::mat4 ComputeWorldMatrix(ObjectId id) const;

    // Undo / Redo + Selective Batch Revert (PLZ-CORE-003, PLZ-UXPLUS-007, PLZ-SAFEX-009, N-06, N-25)
    bool CanUndo() const { return !undoStack_.empty(); }
    bool CanRedo() const { return !redoStack_.empty(); }
    bool Undo(std::string* outLabel = nullptr);
    bool Redo(std::string* outLabel = nullptr);
    bool RevertSpecificBatch(uint64_t batchId, std::string* outError = nullptr);
    const std::vector<UndoBatchEntry>& GetUndoStack() const { return undoStack_; }

    uint64_t RecordMultiObjectBatch(
        const std::string& label,
        const std::string& domain,
        const std::string& author,
        const std::vector<ObjectDiffRecord>& diffs);

    // Asset & Content-Addressed Store Management (PLZ-CORE-004, PLZ-CORE-005, N-03, N-04, N-05)
    const std::vector<AssetMetadata>& GetAssets() const { return assets_; }
    const AssetMetadata* FindAsset(AssetId id) const;
    AssetMetadata* FindAssetMutable(AssetId id);

    std::optional<AssetId> RegisterBinaryAsset(
        const std::string& logicalName,
        const std::string& assetType,
        std::span<const uint8_t> payload,
        const std::string& sourceRelPath = {},
        const std::string& lineageJson = "{}",
        const std::string& licenseInfo = "PLATZ-Internal",
        std::string* outError = nullptr);

    // Updates an asset's payload and invalidates only dependent objects/caches (N-05 / PLZ-ARCH-004)
    bool UpdateAssetPayload(
        AssetId assetId,
        std::span<const uint8_t> newPayload,
        std::vector<ObjectId>* outInvalidatedDependents = nullptr,
        std::string* outError = nullptr);

    std::vector<ObjectId> GetDependentObjectsForAsset(AssetId assetId) const;

    const ContentAddressedAssetStore& GetAssetStore() const { return assetStore_; }
    ProjectDatabase& GetDatabase() { return db_; }
    const ProjectDatabase& GetDatabase() const { return db_; }

    // Portable Export with Rights/Provenance Verification (PLZ-RUNTIME-006, PLZ-PIPE-006, N-27, N-28)
    ExportProvenanceReport ExportPortablePackage(
        const std::filesystem::path& outputJsonFile,
        bool blockOnMissingLicense = true) const;

private:
    void ApplyDiffRecords(const std::vector<ObjectDiffRecord>& diffs, bool forward);

    std::filesystem::path projectDir_;
    std::string projectName_{"PLATZ-Project"};
    std::string activeBranch_{"main"};
    RevisionId headRevision_{0};
    bool dirty_{false};

    std::vector<SceneObject> objects_;
    std::vector<AssetMetadata> assets_;

    std::vector<UndoBatchEntry> undoStack_;
    std::vector<UndoBatchEntry> redoStack_;
    uint64_t nextBatchId_{1};

    ContentAddressedAssetStore assetStore_;
    ProjectDatabase db_;
};

} // namespace platz::core
