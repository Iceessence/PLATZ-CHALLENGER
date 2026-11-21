#include "core/project_state.h"
#include "platform/platform_paths.h"

#include <algorithm>
#include <fstream>

namespace platz::core {

ProjectState::ProjectState() {
    objects_.reserve(256);
    assets_.reserve(64);
}

bool ProjectState::CreateOrOpenProject(
    const std::filesystem::path& projectDir,
    const std::string& defaultName,
    std::string* outError) {
    std::error_code ec;
    std::filesystem::create_directories(projectDir, ec);
    if (ec) {
        if (outError) *outError = "Failed creating project directory: " + ec.message();
        return false;
    }

    projectDir_ = std::filesystem::weakly_canonical(projectDir, ec);
    if (ec) projectDir_ = projectDir;

    assetStore_.SetProjectRoot(projectDir_);
    if (!assetStore_.EnsureDirectories(outError)) {
        return false;
    }
    // Clean up any orphaned staging files left by a prior interrupted write (N-03)
    assetStore_.CleanupOrphanedStagingFiles();

    std::filesystem::path dbFile = projectDir_ / "project.platzdb";
    if (!db_.OpenOrCreate(dbFile, defaultName, outError)) {
        return false;
    }

    if (!ReloadFromDisk(std::nullopt, outError)) {
        return false;
    }

    undoStack_.clear();
    redoStack_.clear();
    dirty_ = false;
    return true;
}

bool ProjectState::SaveProject(
    const std::string& summary,
    const std::string& author,
    std::string* outError,
    bool injectFaultBeforeCommit) {
    if (!db_.IsOpen()) {
        if (outError) *outError = "Project database is not open";
        return false;
    }

    // Verify all referenced asset blobs exist in the content-addressed store before advancing revision head (PLZ-ARCH-001)
    for (const auto& ast : assets_) {
        if (!ast.contentSha256.empty() && !assetStore_.HasVerifiedBlob(ast.contentSha256)) {
            if (outError) {
                *outError = "Asset blob integrity failure before revision commit for asset '" +
                            ast.logicalName + "' (SHA-256: " + ast.contentSha256 + ")";
            }
            return false;
        }
    }

    RevisionId newRev = 0;
    if (!db_.CommitRevisionTransaction(
            projectName_,
            headRevision_,
            activeBranch_,
            author,
            summary,
            objects_,
            assets_,
            &newRev,
            outError,
            injectFaultBeforeCommit)) {
        return false;
    }

    headRevision_ = newRev;
    dirty_ = false;
    return true;
}

bool ProjectState::ReloadFromDisk(std::optional<RevisionId> specificRevision, std::string* outError) {
    if (!db_.IsOpen()) {
        if (outError) *outError = "Project database is not open";
        return false;
    }
    assetStore_.CleanupOrphanedStagingFiles();

    RevisionId loadedRev = 0;
    std::string loadedName;
    std::vector<SceneObject> loadedObjs;
    std::vector<AssetMetadata> loadedAssets;
    if (!db_.LoadRevisionState(specificRevision, &loadedRev, &loadedName, &loadedObjs, &loadedAssets, outError)) {
        return false;
    }
    headRevision_ = loadedRev;
    if (!loadedName.empty()) {
        projectName_ = loadedName;
    }
    objects_ = std::move(loadedObjs);
    assets_ = std::move(loadedAssets);
    dirty_ = false;
    ++mutationGeneration_;
    return true;
}

SceneObject* ProjectState::FindObject(ObjectId id) {
    for (auto& obj : objects_) {
        if (obj.id == id) return &obj;
    }
    return nullptr;
}

const SceneObject* ProjectState::FindObject(ObjectId id) const {
    for (const auto& obj : objects_) {
        if (obj.id == id) return &obj;
    }
    return nullptr;
}

std::vector<ObjectId> ProjectState::FindObjectsByName(std::string_view name) const {
    std::vector<ObjectId> matches;
    for (const auto& obj : objects_) {
        if (obj.name == name) {
            matches.push_back(obj.id);
        }
    }
    return matches;
}

SceneObject& ProjectState::CreateObject(
    const std::string& name,
    const std::string& primitiveType,
    ObjectId parentId,
    const std::string& domain,
    const std::string& author) {
    SceneObject obj;
    obj.id = GenerateObjectId();
    obj.name = name;
    obj.parentId = parentId;
    obj.mesh.primitiveType = primitiveType;
    obj.transform.Sanitize();

    ObjectDiffRecord diff;
    diff.objectId = obj.id;
    diff.existedBefore = false;
    diff.existsAfter = true;
    diff.afterJson = obj.ToJson();

    objects_.push_back(obj);
    RecordMultiObjectBatch("Create " + name, domain, author, {diff});
    dirty_ = true;
    ++mutationGeneration_;
    return objects_.back();
}

std::optional<ObjectId> ProjectState::DuplicateObject(ObjectId sourceId, const std::string& author) {
    const SceneObject* src = FindObject(sourceId);
    if (!src) return std::nullopt;

    SceneObject copy = *src;
    copy.id = GenerateObjectId();
    copy.name = src->name + " (Copy)";
    copy.transform.position += glm::vec3(1.25f, 0.0f, 1.25f);

    ObjectDiffRecord diff;
    diff.objectId = copy.id;
    diff.existedBefore = false;
    diff.existsAfter = true;
    diff.afterJson = copy.ToJson();

    objects_.push_back(copy);
    RecordMultiObjectBatch("Duplicate " + src->name, "Scene", author, {diff});
    dirty_ = true;
    ++mutationGeneration_;
    return copy.id;
}

bool ProjectState::DeleteObject(ObjectId id, const std::string& author) {
    auto it = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& o) { return o.id == id; });
    if (it == objects_.end()) return false;

    ObjectDiffRecord diff;
    diff.objectId = id;
    diff.existedBefore = true;
    diff.existsAfter = false;
    diff.beforeJson = it->ToJson();

    std::string deletedName = it->name;
    objects_.erase(it);

    // Reparent direct children to root cleanly
    for (auto& child : objects_) {
        if (child.parentId == id) {
            child.parentId = 0;
        }
    }

    RecordMultiObjectBatch("Delete " + deletedName, "Scene", author, {diff});
    dirty_ = true;
    ++mutationGeneration_;
    return true;
}

bool ProjectState::ApplyObjectMutation(
    ObjectId id,
    const SceneObject& updated,
    const std::string& actionLabel,
    const std::string& domain,
    const std::string& author,
    uint64_t* outBatchId) {
    SceneObject* existing = FindObject(id);
    if (!existing) return false;

    ObjectDiffRecord diff;
    diff.objectId = id;
    diff.existedBefore = true;
    diff.existsAfter = true;
    diff.beforeJson = existing->ToJson();

    SceneObject sanitized = updated;
    sanitized.id = id;
    sanitized.transform.Sanitize();
    diff.afterJson = sanitized.ToJson();

    *existing = std::move(sanitized);
    uint64_t bid = RecordMultiObjectBatch(actionLabel, domain, author, {diff});
    if (outBatchId) *outBatchId = bid;
    dirty_ = true;
    ++mutationGeneration_;
    return true;
}

glm::mat4 ProjectState::ComputeWorldMatrix(ObjectId id) const {
    const SceneObject* cur = FindObject(id);
    if (!cur) return glm::mat4(1.0f);

    glm::mat4 world = cur->transform.ToLocalMatrix();
    ObjectId pid = cur->parentId;
    int depthGuard = 0;
    while (pid != 0 && depthGuard < 32) {
        const SceneObject* parent = FindObject(pid);
        if (!parent || parent->id == id) break;
        world = parent->transform.ToLocalMatrix() * world;
        pid = parent->parentId;
        ++depthGuard;
    }
    return world;
}

uint64_t ProjectState::RecordMultiObjectBatch(
    const std::string& label,
    const std::string& domain,
    const std::string& author,
    const std::vector<ObjectDiffRecord>& diffs) {
    UndoBatchEntry batch;
    batch.batchId = nextBatchId_++;
    batch.label = label;
    batch.domain = domain;
    batch.author = author;
    batch.objectDiffs = diffs;
    undoStack_.push_back(std::move(batch));
    redoStack_.clear();
    return undoStack_.back().batchId;
}

void ProjectState::ApplyDiffRecords(const std::vector<ObjectDiffRecord>& diffs, bool forward) {
    if (forward) {
        for (const auto& d : diffs) {
            if (!d.existedBefore && d.existsAfter) {
                SceneObject obj = SceneObject::FromJson(d.afterJson);
                if (SceneObject* ex = FindObject(obj.id)) {
                    *ex = std::move(obj);
                } else {
                    objects_.push_back(std::move(obj));
                }
            } else if (d.existedBefore && !d.existsAfter) {
                std::erase_if(objects_, [&](const SceneObject& o) { return o.id == d.objectId; });
            } else if (d.existedBefore && d.existsAfter) {
                if (SceneObject* ex = FindObject(d.objectId)) {
                    *ex = SceneObject::FromJson(d.afterJson);
                }
            }
        }
    } else {
        for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
            const auto& d = *it;
            if (!d.existedBefore && d.existsAfter) {
                std::erase_if(objects_, [&](const SceneObject& o) { return o.id == d.objectId; });
            } else if (d.existedBefore && !d.existsAfter) {
                SceneObject obj = SceneObject::FromJson(d.beforeJson);
                if (SceneObject* ex = FindObject(obj.id)) {
                    *ex = std::move(obj);
                } else {
                    objects_.push_back(std::move(obj));
                }
            } else if (d.existedBefore && d.existsAfter) {
                if (SceneObject* ex = FindObject(d.objectId)) {
                    *ex = SceneObject::FromJson(d.beforeJson);
                }
            }
        }
    }
    dirty_ = true;
    ++mutationGeneration_;
}

bool ProjectState::Undo(std::string* outLabel) {
    if (undoStack_.empty()) return false;
    UndoBatchEntry top = std::move(undoStack_.back());
    undoStack_.pop_back();
    ApplyDiffRecords(top.objectDiffs, false);
    if (outLabel) *outLabel = top.label;
    redoStack_.push_back(std::move(top));
    return true;
}

bool ProjectState::Redo(std::string* outLabel) {
    if (redoStack_.empty()) return false;
    UndoBatchEntry top = std::move(redoStack_.back());
    redoStack_.pop_back();
    ApplyDiffRecords(top.objectDiffs, true);
    if (outLabel) *outLabel = top.label;
    undoStack_.push_back(std::move(top));
    return true;
}

bool ProjectState::RevertSpecificBatch(uint64_t batchId, std::string* outError) {
    auto it = std::find_if(undoStack_.begin(), undoStack_.end(),
        [batchId](const UndoBatchEntry& b) { return b.batchId == batchId; });
    if (it == undoStack_.end()) {
        if (outError) *outError = "Undo batch " + std::to_string(batchId) + " not found";
        return false;
    }

    // Apply only that batch's inverse object diffs without rolling back independent changes to other objects (PLZ-SAFEX-009 / N-25)
    std::vector<ObjectDiffRecord> inverseDiffs;
    for (const auto& d : it->objectDiffs) {
        ObjectDiffRecord inv;
        inv.objectId = d.objectId;
        if (const SceneObject* cur = FindObject(d.objectId)) {
            inv.existedBefore = true;
            inv.beforeJson = cur->ToJson();
        } else {
            inv.existedBefore = false;
        }
        inv.existsAfter = d.existedBefore;
        inv.afterJson = d.beforeJson;
        inverseDiffs.push_back(inv);
    }

    ApplyDiffRecords(it->objectDiffs, false);
    undoStack_.erase(it);
    RecordMultiObjectBatch("Revert Batch #" + std::to_string(batchId), "AI", "System", inverseDiffs);
    return true;
}

const AssetMetadata* ProjectState::FindAsset(AssetId id) const {
    for (const auto& a : assets_) {
        if (a.assetId == id) return &a;
    }
    return nullptr;
}

AssetMetadata* ProjectState::FindAssetMutable(AssetId id) {
    for (auto& a : assets_) {
        if (a.assetId == id) return &a;
    }
    return nullptr;
}

std::optional<AssetId> ProjectState::RegisterBinaryAsset(
    const std::string& logicalName,
    const std::string& assetType,
    std::span<const uint8_t> payload,
    const std::string& sourceRelPath,
    const std::string& lineageJson,
    const std::string& licenseInfo,
    std::string* outError) {
    std::string sha256;
    if (!assetStore_.StoreBlob(payload, &sha256, outError)) {
        return std::nullopt;
    }

    AssetMetadata meta;
    meta.assetId = GenerateAssetId();
    meta.logicalName = logicalName;
    meta.assetType = assetType;
    meta.contentSha256 = sha256;
    meta.byteSize = payload.size();
    meta.sourceRelPath = sourceRelPath;
    meta.lineageJson = lineageJson;
    meta.licenseInfo = licenseInfo;
    meta.generationVersion = 1;

    assets_.push_back(meta);
    dirty_ = true;
    ++mutationGeneration_;
    return meta.assetId;
}

std::vector<ObjectId> ProjectState::GetDependentObjectsForAsset(AssetId assetId) const {
    std::vector<ObjectId> deps;
    if (assetId == 0) return deps;
    for (const auto& obj : objects_) {
        if (obj.mesh.meshAssetId == assetId ||
            obj.material.albedoTextureAssetId == assetId ||
            obj.material.pixelSourceAssetId == assetId) {
            deps.push_back(obj.id);
        }
    }
    return deps;
}

bool ProjectState::UpdateAssetPayload(
    AssetId assetId,
    std::span<const uint8_t> newPayload,
    std::vector<ObjectId>* outInvalidatedDependents,
    std::string* outError) {
    AssetMetadata* meta = FindAssetMutable(assetId);
    if (!meta) {
        if (outError) *outError = "Asset ID not found";
        return false;
    }
    std::string newSha;
    if (!assetStore_.StoreBlob(newPayload, &newSha, outError)) {
        return false;
    }
    meta->contentSha256 = newSha;
    meta->byteSize = newPayload.size();
    meta->generationVersion += 1;
    dirty_ = true;
    ++mutationGeneration_;

    if (outInvalidatedDependents) {
        *outInvalidatedDependents = GetDependentObjectsForAsset(assetId);
    }
    return true;
}

ExportProvenanceReport ProjectState::ExportPortablePackage(
    const std::filesystem::path& outputJsonFile,
    bool blockOnMissingLicense) const {
    ExportProvenanceReport rep;
    rep.outputBundlePath = outputJsonFile;
    rep.sourceRevision = headRevision_;
    rep.objectCount = objects_.size();
    rep.assetCount = assets_.size();

    for (const auto& ast : assets_) {
        if (ast.licenseInfo.empty() ||
            ast.licenseInfo == "UNKNOWN" ||
            ast.licenseInfo == "UNLICENSED" ||
            ast.licenseInfo == "MISSING_RIGHTS") {
            rep.unlicensedAssets.push_back(ast.logicalName + " (" + FormatAssetId(ast.assetId) + ")");
        }
    }

    if (blockOnMissingLicense && !rep.unlicensedAssets.empty()) {
        rep.allowed = false;
        rep.errorMessage = "Export blocked by asset provenance policy (PLZ-PIPE-006 / N-28): missing rights on " +
                           std::to_string(rep.unlicensedAssets.size()) + " asset(s).";
        return rep;
    }

    nlohmann::json bundle;
    bundle["product"] = "PLATZ";
    bundle["bundleVersion"] = "1.0.0";
    bundle["projectName"] = projectName_;
    bundle["sourceRevision"] = headRevision_;
    bundle["exportedUtc"] = platform::UtcIso8601Now();
    bundle["targetPlatform"] = "Windows-x64-Vulkan-1.4";
    bundle["prerequisites"] = {
        {"os", "Windows 10/11 x64"},
        {"graphicsApi", "Vulkan 1.4 (Dynamic Rendering, Synchronization2)"},
        {"runtimeDlls", nlohmann::json::array({"SDL3.dll", "slang.dll"})}
    };

    nlohmann::json objsArr = nlohmann::json::array();
    for (const auto& o : objects_) objsArr.push_back(o.ToJson());
    bundle["objects"] = std::move(objsArr);

    nlohmann::json astArr = nlohmann::json::array();
    for (const auto& a : assets_) astArr.push_back(a.ToJson());
    bundle["assets"] = std::move(astArr);

    std::error_code ec;
    if (!outputJsonFile.parent_path().empty()) {
        std::filesystem::create_directories(outputJsonFile.parent_path(), ec);
    }
    std::ofstream ofs(outputJsonFile, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        rep.allowed = false;
        rep.errorMessage = "Failed opening export destination: " + outputJsonFile.string();
        return rep;
    }
    ofs << bundle.dump(2);
    ofs.flush();
    rep.allowed = ofs.good();
    return rep;
}

} // namespace platz::core
