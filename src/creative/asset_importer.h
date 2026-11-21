#pragma once

#include "core/project_state.h"
#include "creative/mesh_generator.h"

#include <filesystem>
#include <optional>
#include <string>

namespace platz::creative {

struct AssetImportResult {
    bool success{false};
    core::AssetId assetId{0};
    MeshData meshData;
    std::string contentSha256;
    std::string diagnosticMessage;
};

class AssetImporter {
public:
    // Parse Wavefront .obj text or file into MeshData
    static bool LoadObjFromString(
        const std::string& objContent,
        const std::string& logicalName,
        MeshData* outMesh,
        std::string* outError = nullptr);

    static bool LoadObjFromFile(
        const std::filesystem::path& filePath,
        MeshData* outMesh,
        std::string* outError = nullptr);

    // Export MeshData to Wavefront .obj for loss-aware interchange round-trip verification (J-03, PLZ-ARCH-007)
    static std::string ExportMeshToObjString(const MeshData& mesh);
    static bool ExportMeshToObjFile(
        const MeshData& mesh,
        const std::filesystem::path& outputPath,
        std::string* outError = nullptr);

    // Parse glTF 2.0 (.gltf or .glb) file into MeshData
    static bool LoadGltfFromFile(
        const std::filesystem::path& filePath,
        MeshData* outMesh,
        std::string* outError = nullptr);

    // Full import into ProjectState content-addressed asset store + optional SceneObject creation
    static AssetImportResult ImportMeshIntoProject(
        core::ProjectState& project,
        const MeshData& mesh,
        const std::string& logicalName,
        const std::string& sourceRelPath = {},
        const std::string& licenseInfo = "CC0-1.0",
        core::ObjectId* outSpawnedObjectId = nullptr);
};

} // namespace platz::creative
