#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platz::platform {

struct PathSecurityResult {
    bool allowed{false};
    std::filesystem::path resolvedPath;
    std::string violationReason;
};

// FIPS 180-4 SHA-256 utilities
std::string ComputeSha256Hex(std::span<const uint8_t> data);
std::string ComputeSha256Hex(std::string_view text);
std::string ComputeFileSha256Hex(const std::filesystem::path& filePath, bool* outOk = nullptr);

// Safe path confinement (PLZ-SAFEX-003, N-20):
// Rejects '..', NUL characters, drive-switching, symlinks, and Windows junctions/reparse points
// that escape outside rootDir.
PathSecurityResult ResolveConfinedPath(
    const std::filesystem::path& rootDir,
    const std::filesystem::path& candidatePath);

bool IsReparsePointOrSymlink(const std::filesystem::path& p);

std::filesystem::path GetExecutableDir();
std::filesystem::path GetRepoRootDir();
std::string UtcIso8601Now();

} // namespace platz::platform
