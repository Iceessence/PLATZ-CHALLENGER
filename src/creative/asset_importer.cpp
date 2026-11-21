#include "creative/asset_importer.h"
#include "platform/platform_paths.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <fstream>
#include <sstream>

namespace platz::creative {

bool AssetImporter::LoadObjFromString(
    const std::string& objContent,
    const std::string& logicalName,
    MeshData* outMesh,
    std::string* outError) {
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = true;

    if (!reader.ParseFromString(objContent, "", config)) {
        if (outError) {
            *outError = "OBJ parse failed: " + reader.Error();
        }
        return false;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    if (attrib.vertices.empty() || shapes.empty()) {
        if (outError) *outError = "OBJ contains no valid geometry or faces";
        return false;
    }

    MeshData mesh;
    mesh.name = logicalName;

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            size_t fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);
            if (fv < 3) {
                indexOffset += fv;
                continue;
            }
            uint32_t triVerts[3]{};
            for (size_t v = 0; v < 3; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                Vertex3D vert{};
                if (idx.vertex_index >= 0 && static_cast<size_t>(3 * idx.vertex_index + 2) < attrib.vertices.size()) {
                    vert.position = {
                        attrib.vertices[3 * idx.vertex_index + 0],
                        attrib.vertices[3 * idx.vertex_index + 1],
                        attrib.vertices[3 * idx.vertex_index + 2]
                    };
                }
                if (idx.normal_index >= 0 && static_cast<size_t>(3 * idx.normal_index + 2) < attrib.normals.size()) {
                    vert.normal = {
                        attrib.normals[3 * idx.normal_index + 0],
                        attrib.normals[3 * idx.normal_index + 1],
                        attrib.normals[3 * idx.normal_index + 2]
                    };
                }
                if (idx.texcoord_index >= 0 && static_cast<size_t>(2 * idx.texcoord_index + 1) < attrib.texcoords.size()) {
                    vert.uv = {
                        attrib.texcoords[2 * idx.texcoord_index + 0],
                        attrib.texcoords[2 * idx.texcoord_index + 1]
                    };
                }
                if (idx.vertex_index >= 0 && static_cast<size_t>(3 * idx.vertex_index + 2) < attrib.colors.size()) {
                    vert.color = {
                        attrib.colors[3 * idx.vertex_index + 0],
                        attrib.colors[3 * idx.vertex_index + 1],
                        attrib.colors[3 * idx.vertex_index + 2]
                    };
                } else {
                    vert.color = glm::vec3(1.0f);
                }

                triVerts[v] = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vert);
                mesh.indices.push_back(triVerts[v]);
            }

            // Compute flat normal if OBJ omitted vertex normals
            if (glm::length(mesh.vertices[triVerts[0]].normal) < 1e-4f) {
                glm::vec3 e1 = mesh.vertices[triVerts[1]].position - mesh.vertices[triVerts[0]].position;
                glm::vec3 e2 = mesh.vertices[triVerts[2]].position - mesh.vertices[triVerts[0]].position;
                glm::vec3 fn = glm::cross(e1, e2);
                float len = glm::length(fn);
                fn = (len > 1e-6f) ? (fn / len) : glm::vec3(0.0f, 1.0f, 0.0f);
                mesh.vertices[triVerts[0]].normal = fn;
                mesh.vertices[triVerts[1]].normal = fn;
                mesh.vertices[triVerts[2]].normal = fn;
            }

            indexOffset += fv;
        }
    }

    mesh.RecomputeBounds();
    if (!mesh.ValidateTopology(outError)) {
        return false;
    }
    if (outMesh) *outMesh = std::move(mesh);
    return true;
}

bool AssetImporter::LoadObjFromFile(
    const std::filesystem::path& filePath,
    MeshData* outMesh,
    std::string* outError) {
    std::ifstream ifs(filePath);
    if (!ifs) {
        if (outError) *outError = "Cannot open OBJ file: " + filePath.string();
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return LoadObjFromString(oss.str(), filePath.stem().string(), outMesh, outError);
}

std::string AssetImporter::ExportMeshToObjString(const MeshData& mesh) {
    std::ostringstream oss;
    oss << "# PLATZ Studio Wavefront OBJ Export (" << mesh.name << ")\n";
    oss << "o " << (mesh.name.empty() ? "PlatzMesh" : mesh.name) << "\n";
    for (const auto& v : mesh.vertices) {
        oss << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
    }
    for (const auto& v : mesh.vertices) {
        oss << "vt " << v.uv.x << " " << v.uv.y << "\n";
    }
    for (const auto& v : mesh.vertices) {
        oss << "vn " << v.normal.x << " " << v.normal.y << " " << v.normal.z << "\n";
    }
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i] + 1;
        uint32_t i1 = mesh.indices[i + 1] + 1;
        uint32_t i2 = mesh.indices[i + 2] + 1;
        oss << "f " << i0 << "/" << i0 << "/" << i0 << " "
            << i1 << "/" << i1 << "/" << i1 << " "
            << i2 << "/" << i2 << "/" << i2 << "\n";
    }
    return oss.str();
}

bool AssetImporter::ExportMeshToObjFile(
    const MeshData& mesh,
    const std::filesystem::path& outputPath,
    std::string* outError) {
    std::ofstream ofs(outputPath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (outError) *outError = "Failed opening output OBJ file: " + outputPath.string();
        return false;
    }
    ofs << ExportMeshToObjString(mesh);
    ofs.flush();
    return ofs.good();
}

bool AssetImporter::LoadGltfFromFile(
    const std::filesystem::path& filePath,
    MeshData* outMesh,
    std::string* outError) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    std::string pathStr = filePath.string();
    cgltf_result res = cgltf_parse_file(&options, pathStr.c_str(), &data);
    if (res != cgltf_result_success || !data) {
        if (outError) *outError = "cgltf_parse_file failed for " + pathStr;
        return false;
    }
    res = cgltf_load_buffers(&options, data, pathStr.c_str());
    if (res != cgltf_result_success) {
        cgltf_free(data);
        if (outError) *outError = "cgltf_load_buffers failed for " + pathStr;
        return false;
    }

    MeshData mesh;
    mesh.name = filePath.stem().string();

    for (cgltf_size m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& gm = data->meshes[m];
        for (cgltf_size p = 0; p < gm.primitives_count; ++p) {
            const cgltf_primitive& prim = gm.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* posAcc = nullptr;
            const cgltf_accessor* normAcc = nullptr;
            const cgltf_accessor* uvAcc = nullptr;

            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) posAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) normAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvAcc = attr.data;
            }
            if (!posAcc) continue;

            uint32_t baseVertex = static_cast<uint32_t>(mesh.vertices.size());
            for (cgltf_size v = 0; v < posAcc->count; ++v) {
                Vertex3D vert{};
                float p3[3]{0, 0, 0};
                cgltf_accessor_read_float(posAcc, v, p3, 3);
                vert.position = {p3[0], p3[1], p3[2]};

                if (normAcc && v < normAcc->count) {
                    float n3[3]{0, 1, 0};
                    cgltf_accessor_read_float(normAcc, v, n3, 3);
                    vert.normal = {n3[0], n3[1], n3[2]};
                } else {
                    vert.normal = {0.0f, 1.0f, 0.0f};
                }
                if (uvAcc && v < uvAcc->count) {
                    float uv2[2]{0, 0};
                    cgltf_accessor_read_float(uvAcc, v, uv2, 2);
                    vert.uv = {uv2[0], uv2[1]};
                }
                vert.color = glm::vec3(1.0f);
                mesh.vertices.push_back(vert);
            }

            if (prim.indices) {
                for (cgltf_size idx = 0; idx < prim.indices->count; ++idx) {
                    mesh.indices.push_back(baseVertex + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx)));
                }
            } else {
                for (cgltf_size idx = 0; idx < posAcc->count; ++idx) {
                    mesh.indices.push_back(baseVertex + static_cast<uint32_t>(idx));
                }
            }
        }
    }

    cgltf_free(data);

    mesh.RecomputeBounds();
    if (!mesh.ValidateTopology(outError)) {
        return false;
    }
    if (outMesh) *outMesh = std::move(mesh);
    return true;
}

AssetImportResult AssetImporter::ImportMeshIntoProject(
    core::ProjectState& project,
    const MeshData& mesh,
    const std::string& logicalName,
    const std::string& sourceRelPath,
    const std::string& licenseInfo,
    core::ObjectId* outSpawnedObjectId) {
    AssetImportResult res{};
    if (!mesh.ValidateTopology(&res.diagnosticMessage)) {
        return res;
    }

    std::vector<uint8_t> binaryBlob = mesh.SerializeToBinaryBlob();
    nlohmann::json lineage = {
        {"importer", "PLATZ-AssetImporter-v1"},
        {"vertexCount", mesh.vertices.size()},
        {"indexCount", mesh.indices.size()},
        {"triangleCount", mesh.indices.size() / 3},
        {"sourceRelPath", sourceRelPath},
        {"importedUtc", platform::UtcIso8601Now()}
    };

    std::string err;
    auto assetIdOpt = project.RegisterBinaryAsset(
        logicalName,
        "mesh",
        binaryBlob,
        sourceRelPath,
        lineage.dump(),
        licenseInfo,
        &err);
    if (!assetIdOpt) {
        res.diagnosticMessage = err;
        return res;
    }

    res.success = true;
    res.assetId = *assetIdOpt;
    res.meshData = mesh;
    if (const auto* meta = project.FindAsset(res.assetId)) {
        res.contentSha256 = meta->contentSha256;
    }

    if (outSpawnedObjectId) {
        auto& obj = project.CreateObject(logicalName, "custom_asset", 0, "Asset", "Importer");
        obj.mesh.primitiveType = "custom_asset";
        obj.mesh.meshAssetId = res.assetId;
        *outSpawnedObjectId = obj.id;
    }
    return res;
}

} // namespace platz::creative
