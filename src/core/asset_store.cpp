#include "core/asset_store.h"
#include "platform/platform_paths.h"

#include <atomic>
#include <fstream>

namespace platz::core {

namespace {
std::atomic<uint64_t> gStagingCounter{1};
} // namespace

nlohmann::json AssetMetadata::ToJson() const {
    return {
        {"assetId", assetId},
        {"logicalName", logicalName},
        {"assetType", assetType},
        {"contentSha256", contentSha256},
        {"byteSize", byteSize},
        {"sourceRelPath", sourceRelPath},
        {"lineageJson", lineageJson},
        {"licenseInfo", licenseInfo},
        {"generationVersion", generationVersion}
    };
}

AssetMetadata AssetMetadata::FromJson(const nlohmann::json& j) {
    AssetMetadata a;
    a.assetId = j.value("assetId", AssetId{0});
    a.logicalName = j.value("logicalName", std::string("Asset"));
    a.assetType = j.value("assetType", std::string("mesh"));
    a.contentSha256 = j.value("contentSha256", std::string(""));
    a.byteSize = j.value("byteSize", uint64_t{0});
    a.sourceRelPath = j.value("sourceRelPath", std::string(""));
    a.lineageJson = j.value("lineageJson", std::string("{}"));
    a.licenseInfo = j.value("licenseInfo", std::string("PLATZ-Internal"));
    a.generationVersion = j.value("generationVersion", uint64_t{1});
    return a;
}

ContentAddressedAssetStore::ContentAddressedAssetStore(std::filesystem::path projectRootDir)
    : projectRoot_(std::move(projectRootDir)) {}

void ContentAddressedAssetStore::SetProjectRoot(const std::filesystem::path& projectRootDir) {
    projectRoot_ = projectRootDir;
}

bool ContentAddressedAssetStore::EnsureDirectories(std::string* outError) const {
    if (projectRoot_.empty()) {
        if (outError) *outError = "Project root is empty";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(projectRoot_ / "assets" / "blobs" / "sha256", ec);
    if (ec) {
        if (outError) *outError = "Failed creating blob directory: " + ec.message();
        return false;
    }
    std::filesystem::create_directories(projectRoot_ / "assets" / "staging", ec);
    if (ec) {
        if (outError) *outError = "Failed creating staging directory: " + ec.message();
        return false;
    }
    std::filesystem::create_directories(projectRoot_ / "assets" / "cache", ec);
    return true;
}

std::filesystem::path ContentAddressedAssetStore::GetBlobPath(const std::string& sha256) const {
    std::string prefix = sha256.size() >= 2 ? sha256.substr(0, 2) : "00";
    return projectRoot_ / "assets" / "blobs" / "sha256" / prefix / (sha256 + ".blob");
}

StagedBlobTicket ContentAddressedAssetStore::StageBlob(std::span<const uint8_t> payload) const {
    StagedBlobTicket ticket{};
    std::string err;
    if (!EnsureDirectories(&err)) {
        ticket.errorMessage = err;
        return ticket;
    }

    ticket.contentSha256 = platform::ComputeSha256Hex(payload);
    ticket.byteSize = payload.size();
    uint64_t seq = gStagingCounter.fetch_add(1, std::memory_order_relaxed);
    ticket.stagingFilePath = projectRoot_ / "assets" / "staging" /
                             (ticket.contentSha256 + "_" + std::to_string(seq) + ".tmp");

    {
        std::ofstream ofs(ticket.stagingFilePath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            ticket.errorMessage = "Unable to open staging file for writing: " + ticket.stagingFilePath.string();
            return ticket;
        }
        if (!payload.empty()) {
            ofs.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }
        ofs.flush();
        if (!ofs.good()) {
            ticket.errorMessage = "I/O error writing staging file";
            return ticket;
        }
    }

    bool ok = false;
    std::string diskHash = platform::ComputeFileSha256Hex(ticket.stagingFilePath, &ok);
    if (!ok || diskHash != ticket.contentSha256) {
        std::error_code ec;
        std::filesystem::remove(ticket.stagingFilePath, ec);
        ticket.errorMessage = "Staged blob SHA-256 verification failed on disk";
        return ticket;
    }

    ticket.valid = true;
    return ticket;
}

bool ContentAddressedAssetStore::PromoteStagedBlob(const StagedBlobTicket& ticket, std::string* outError) const {
    if (!ticket.valid || ticket.contentSha256.size() != 64) {
        if (outError) *outError = "Invalid staged blob ticket";
        return false;
    }

    std::filesystem::path finalPath = GetBlobPath(ticket.contentSha256);
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec) {
        if (outError) *outError = "Failed creating blob shard directory: " + ec.message();
        return false;
    }

    if (std::filesystem::exists(finalPath, ec) && HasVerifiedBlob(ticket.contentSha256)) {
        // Content-addressed deduplication: identical verified blob already exists
        std::filesystem::remove(ticket.stagingFilePath, ec);
        return true;
    }

    std::filesystem::rename(ticket.stagingFilePath, finalPath, ec);
    if (ec) {
        std::filesystem::copy_file(ticket.stagingFilePath, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rmEc;
        std::filesystem::remove(ticket.stagingFilePath, rmEc);
        if (ec) {
            if (outError) *outError = "Failed promoting staged blob to content-addressed store: " + ec.message();
            return false;
        }
    }

    if (!HasVerifiedBlob(ticket.contentSha256)) {
        if (outError) *outError = "Promoted blob failed post-move SHA-256 check";
        return false;
    }
    return true;
}

bool ContentAddressedAssetStore::StoreBlob(
    std::span<const uint8_t> payload,
    std::string* outSha256,
    std::string* outError) const {
    StagedBlobTicket ticket = StageBlob(payload);
    if (!ticket.valid) {
        if (outError) *outError = ticket.errorMessage;
        return false;
    }
    if (!PromoteStagedBlob(ticket, outError)) {
        return false;
    }
    if (outSha256) *outSha256 = ticket.contentSha256;
    return true;
}

std::optional<std::vector<uint8_t>> ContentAddressedAssetStore::ReadBlobVerified(
    const std::string& expectedSha256,
    std::string* outError) const {
    if (expectedSha256.size() != 64) {
        if (outError) *outError = "Invalid SHA-256 digest length";
        return std::nullopt;
    }
    std::filesystem::path p = GetBlobPath(expectedSha256);
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) {
        if (outError) *outError = "Blob file missing: " + p.string();
        return std::nullopt;
    }
    auto sz = ifs.tellg();
    if (sz < 0) {
        if (outError) *outError = "Failed reading blob size";
        return std::nullopt;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!buf.empty()) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    std::string actualSha = platform::ComputeSha256Hex(buf);
    if (actualSha != expectedSha256) {
        if (outError) *outError = "Blob corruption detected: expected SHA-256 " + expectedSha256 + ", got " + actualSha;
        return std::nullopt;
    }
    return buf;
}

bool ContentAddressedAssetStore::HasVerifiedBlob(const std::string& expectedSha256) const {
    bool ok = false;
    std::string actual = platform::ComputeFileSha256Hex(GetBlobPath(expectedSha256), &ok);
    return ok && (actual == expectedSha256);
}

size_t ContentAddressedAssetStore::CleanupOrphanedStagingFiles() const {
    size_t removed = 0;
    std::error_code ec;
    std::filesystem::path stagingDir = projectRoot_ / "assets" / "staging";
    if (!std::filesystem::exists(stagingDir, ec)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::directory_iterator(stagingDir, ec)) {
        if (entry.is_regular_file(ec)) {
            if (std::filesystem::remove(entry.path(), ec)) {
                ++removed;
            }
        }
    }
    return removed;
}

} // namespace platz::core
