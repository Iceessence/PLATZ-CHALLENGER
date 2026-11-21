#pragma once

#include "core/object_identity.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace platz::core {

struct AssetMetadata {
    AssetId assetId{0};
    std::string logicalName;
    std::string assetType{"mesh"}; // "mesh", "texture", "material", "shader", "model"
    std::string contentSha256;
    uint64_t byteSize{0};
    std::string sourceRelPath;
    std::string lineageJson{"{}"};
    std::string licenseInfo{"PLATZ-Internal"};
    uint64_t generationVersion{1};

    nlohmann::json ToJson() const;
    static AssetMetadata FromJson(const nlohmann::json& j);
};

struct StagedBlobTicket {
    bool valid{false};
    std::string contentSha256;
    uint64_t byteSize{0};
    std::filesystem::path stagingFilePath;
    std::string errorMessage;
};

class ContentAddressedAssetStore {
public:
    explicit ContentAddressedAssetStore(std::filesystem::path projectRootDir = {});

    void SetProjectRoot(const std::filesystem::path& projectRootDir);
    const std::filesystem::path& GetProjectRoot() const { return projectRoot_; }

    bool EnsureDirectories(std::string* outError = nullptr) const;

    // 1. Stage binary payload to assets/staging/<sha256>_<counter>.tmp and verify disk hash
    StagedBlobTicket StageBlob(std::span<const uint8_t> payload) const;

    // 2. Atomically promote staged file into assets/blobs/sha256/<first2>/<sha256>.blob
    bool PromoteStagedBlob(const StagedBlobTicket& ticket, std::string* outError = nullptr) const;

    // Convenience: Stage + Promote in one call
    bool StoreBlob(std::span<const uint8_t> payload, std::string* outSha256, std::string* outError = nullptr) const;

    // Read blob and verify its SHA-256 matches expectedSha256 (N-04)
    std::optional<std::vector<uint8_t>> ReadBlobVerified(
        const std::string& expectedSha256,
        std::string* outError = nullptr) const;

    bool HasVerifiedBlob(const std::string& expectedSha256) const;
    std::filesystem::path GetBlobPath(const std::string& sha256) const;

    // Crash recovery cleanup of uncommitted staging files (N-03)
    size_t CleanupOrphanedStagingFiles() const;

private:
    std::filesystem::path projectRoot_;
};

} // namespace platz::core
