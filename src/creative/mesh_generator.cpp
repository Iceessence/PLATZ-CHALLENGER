#include "creative/mesh_generator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace platz::creative {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kPlatzMeshMagic = 0x4D5A4C50u; // 'PLZM'
constexpr uint32_t kPlatzMeshVersion = 1u;
} // namespace

bool Aabb3D::IntersectsRay(
    const glm::vec3& rayOrigin,
    const glm::vec3& rayDirNormalized,
    float* outDistance) const {
    float tmin = 0.0f;
    float tmax = 1e30f;

    for (int axis = 0; axis < 3; ++axis) {
        float dir = rayDirNormalized[axis];
        float orig = rayOrigin[axis];
        float bmin = minBounds[axis];
        float bmax = maxBounds[axis];

        if (std::abs(dir) < 1e-7f) {
            if (orig < bmin || orig > bmax) {
                return false;
            }
        } else {
            float invD = 1.0f / dir;
            float t1 = (bmin - orig) * invD;
            float t2 = (bmax - orig) * invD;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) {
                return false;
            }
        }
    }
    if (outDistance) *outDistance = tmin;
    return true;
}

void MeshData::RecomputeBounds() {
    if (vertices.empty()) {
        localBounds.minBounds = glm::vec3(-0.5f);
        localBounds.maxBounds = glm::vec3(0.5f);
        return;
    }
    glm::vec3 bmin = vertices[0].position;
    glm::vec3 bmax = vertices[0].position;
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v.position);
        bmax = glm::max(bmax, v.position);
    }
    // Pad degenerate flat planes slightly for reliable ray picking
    glm::vec3 span = bmax - bmin;
    if (span.x < 0.04f) { bmin.x -= 0.02f; bmax.x += 0.02f; }
    if (span.y < 0.04f) { bmin.y -= 0.02f; bmax.y += 0.02f; }
    if (span.z < 0.04f) { bmin.z -= 0.02f; bmax.z += 0.02f; }
    localBounds.minBounds = bmin;
    localBounds.maxBounds = bmax;
}

bool MeshData::ValidateTopology(std::string* outDiagnostic) const {
    if (vertices.empty() || indices.empty()) {
        if (outDiagnostic) *outDiagnostic = "Mesh has zero vertices or indices";
        return false;
    }
    if (indices.size() % 3 != 0) {
        if (outDiagnostic) *outDiagnostic = "Triangle mesh index count is not divisible by 3";
        return false;
    }
    uint32_t vCount = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vCount) {
            if (outDiagnostic) {
                *outDiagnostic = "Index out of bounds at index[" + std::to_string(i) + "]=" +
                                 std::to_string(indices[i]) + " (vertexCount=" + std::to_string(vCount) + ")";
            }
            return false;
        }
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto& v = vertices[i];
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z) ||
            !std::isfinite(v.normal.x) || !std::isfinite(v.normal.y) || !std::isfinite(v.normal.z)) {
            if (outDiagnostic) *outDiagnostic = "Non-finite position or normal at vertex " + std::to_string(i);
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> MeshData::SerializeToBinaryBlob() const {
    uint32_t vCount = static_cast<uint32_t>(vertices.size());
    uint32_t iCount = static_cast<uint32_t>(indices.size());
    size_t totalBytes = sizeof(uint32_t) * 4 + vCount * sizeof(Vertex3D) + iCount * sizeof(uint32_t);

    std::vector<uint8_t> out(totalBytes);
    uint8_t* ptr = out.data();
    uint32_t header[4] = {kPlatzMeshMagic, kPlatzMeshVersion, vCount, iCount};
    std::memcpy(ptr, header, sizeof(header));
    ptr += sizeof(header);

    if (vCount > 0) {
        std::memcpy(ptr, vertices.data(), vCount * sizeof(Vertex3D));
        ptr += vCount * sizeof(Vertex3D);
    }
    if (iCount > 0) {
        std::memcpy(ptr, indices.data(), iCount * sizeof(uint32_t));
    }
    return out;
}

bool MeshData::DeserializeFromBinaryBlob(
    std::span<const uint8_t> blob,
    MeshData* outMesh,
    std::string* outError) {
    if (blob.size() < sizeof(uint32_t) * 4) {
        if (outError) *outError = "Mesh blob too small for header";
        return false;
    }
    uint32_t header[4]{};
    std::memcpy(header, blob.data(), sizeof(header));
    if (header[0] != kPlatzMeshMagic || header[1] != kPlatzMeshVersion) {
        if (outError) *outError = "Invalid PLATZ mesh binary magic or version";
        return false;
    }
    uint32_t vCount = header[2];
    uint32_t iCount = header[3];
    size_t expectedBytes = sizeof(header) + static_cast<size_t>(vCount) * sizeof(Vertex3D) +
                           static_cast<size_t>(iCount) * sizeof(uint32_t);
    if (blob.size() != expectedBytes) {
        if (outError) *outError = "Mesh blob size mismatch";
        return false;
    }

    if (outMesh) {
        outMesh->vertices.resize(vCount);
        outMesh->indices.resize(iCount);
        const uint8_t* ptr = blob.data() + sizeof(header);
        if (vCount > 0) {
            std::memcpy(outMesh->vertices.data(), ptr, static_cast<size_t>(vCount) * sizeof(Vertex3D));
            ptr += static_cast<size_t>(vCount) * sizeof(Vertex3D);
        }
        if (iCount > 0) {
            std::memcpy(outMesh->indices.data(), ptr, static_cast<size_t>(iCount) * sizeof(uint32_t));
        }
        outMesh->RecomputeBounds();
    }
    return true;
}

MeshData MeshGenerator::GenerateCube(float size, const glm::vec3& vertexColor) {
    MeshData mesh;
    mesh.name = "Cube";
    float h = size * 0.5f;

    struct FaceDef {
        glm::vec3 normal;
        glm::vec3 right;
        glm::vec3 up;
    };
    const FaceDef faces[6] = {
        {{ 0,  0,  1}, { 1, 0, 0}, {0, 1, 0}}, // Front (+Z)
        {{ 0,  0, -1}, {-1, 0, 0}, {0, 1, 0}}, // Back (-Z)
        {{ 1,  0,  0}, { 0, 0,-1}, {0, 1, 0}}, // Right (+X)
        {{-1,  0,  0}, { 0, 0, 1}, {0, 1, 0}}, // Left (-X)
        {{ 0,  1,  0}, { 1, 0, 0}, {0, 0,-1}}, // Top (+Y)
        {{ 0, -1,  0}, { 1, 0, 0}, {0, 0, 1}}  // Bottom (-Y)
    };

    for (const auto& f : faces) {
        uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        glm::vec3 c = f.normal * h;
        glm::vec3 r = f.right * h;
        glm::vec3 u = f.up * h;

        mesh.vertices.push_back({c - r - u, f.normal, {0.0f, 1.0f}, vertexColor});
        mesh.vertices.push_back({c + r - u, f.normal, {1.0f, 1.0f}, vertexColor});
        mesh.vertices.push_back({c + r + u, f.normal, {1.0f, 0.0f}, vertexColor});
        mesh.vertices.push_back({c - r + u, f.normal, {0.0f, 0.0f}, vertexColor});

        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    }

    mesh.RecomputeBounds();
    return mesh;
}

MeshData MeshGenerator::GenerateSphere(float radius, int slices, int stacks, const glm::vec3& vertexColor) {
    MeshData mesh;
    mesh.name = "Sphere";
    slices = std::max(slices, 6);
    stacks = std::max(stacks, 4);

    for (int i = 0; i <= stacks; ++i) {
        float v = static_cast<float>(i) / static_cast<float>(stacks);
        float phi = v * kPi;
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int j = 0; j <= slices; ++j) {
            float u = static_cast<float>(j) / static_cast<float>(slices);
            float theta = u * 2.0f * kPi;
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            glm::vec3 n(cosTheta * sinPhi, cosPhi, sinTheta * sinPhi);
            mesh.vertices.push_back({n * radius, n, {u, v}, vertexColor});
        }
    }

    int ringStride = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t k1 = static_cast<uint32_t>(i * ringStride + j);
            uint32_t k2 = static_cast<uint32_t>((i + 1) * ringStride + j);

            mesh.indices.push_back(k1);
            mesh.indices.push_back(k2);
            mesh.indices.push_back(k1 + 1);

            mesh.indices.push_back(k1 + 1);
            mesh.indices.push_back(k2);
            mesh.indices.push_back(k2 + 1);
        }
    }

    mesh.RecomputeBounds();
    return mesh;
}

MeshData MeshGenerator::GenerateCylinder(float radius, float height, int slices, const glm::vec3& vertexColor) {
    MeshData mesh;
    mesh.name = "Cylinder";
    slices = std::max(slices, 6);
    float halfH = height * 0.5f;

    // Side wall
    for (int i = 0; i <= 1; ++i) {
        float y = (i == 0) ? -halfH : halfH;
        float v = 1.0f - static_cast<float>(i);
        for (int j = 0; j <= slices; ++j) {
            float u = static_cast<float>(j) / static_cast<float>(slices);
            float theta = u * 2.0f * kPi;
            glm::vec3 n(std::cos(theta), 0.0f, std::sin(theta));
            mesh.vertices.push_back({{n.x * radius, y, n.z * radius}, n, {u, v}, vertexColor});
        }
    }
    int stride = slices + 1;
    for (int j = 0; j < slices; ++j) {
        uint32_t b0 = static_cast<uint32_t>(j);
        uint32_t b1 = static_cast<uint32_t>(j + 1);
        uint32_t t0 = static_cast<uint32_t>(stride + j);
        uint32_t t1 = static_cast<uint32_t>(stride + j + 1);

        mesh.indices.push_back(b0);
        mesh.indices.push_back(t0);
        mesh.indices.push_back(b1);

        mesh.indices.push_back(b1);
        mesh.indices.push_back(t0);
        mesh.indices.push_back(t1);
    }

    // Top cap
    uint32_t topCenter = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({{0.0f, halfH, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f}, vertexColor});
    for (int j = 0; j <= slices; ++j) {
        float theta = (static_cast<float>(j) / static_cast<float>(slices)) * 2.0f * kPi;
        float c = std::cos(theta), s = std::sin(theta);
        mesh.vertices.push_back({{c * radius, halfH, s * radius}, {0.0f, 1.0f, 0.0f}, {c * 0.5f + 0.5f, s * 0.5f + 0.5f}, vertexColor});
    }
    for (int j = 0; j < slices; ++j) {
        mesh.indices.push_back(topCenter);
        mesh.indices.push_back(topCenter + 1 + j + 1);
        mesh.indices.push_back(topCenter + 1 + j);
    }

    // Bottom cap
    uint32_t botCenter = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({{0.0f, -halfH, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.5f, 0.5f}, vertexColor});
    for (int j = 0; j <= slices; ++j) {
        float theta = (static_cast<float>(j) / static_cast<float>(slices)) * 2.0f * kPi;
        float c = std::cos(theta), s = std::sin(theta);
        mesh.vertices.push_back({{c * radius, -halfH, s * radius}, {0.0f, -1.0f, 0.0f}, {c * 0.5f + 0.5f, s * 0.5f + 0.5f}, vertexColor});
    }
    for (int j = 0; j < slices; ++j) {
        mesh.indices.push_back(botCenter);
        mesh.indices.push_back(botCenter + 1 + j);
        mesh.indices.push_back(botCenter + 1 + j + 1);
    }

    mesh.RecomputeBounds();
    return mesh;
}

MeshData MeshGenerator::GeneratePlane(float width, float depth, float uvRepeat, const glm::vec3& vertexColor) {
    MeshData mesh;
    mesh.name = "Plane";
    float hw = width * 0.5f;
    float hd = depth * 0.5f;
    glm::vec3 n(0.0f, 1.0f, 0.0f);

    mesh.vertices.push_back({{-hw, 0.0f,  hd}, n, {0.0f,     uvRepeat}, vertexColor});
    mesh.vertices.push_back({{ hw, 0.0f,  hd}, n, {uvRepeat, uvRepeat}, vertexColor});
    mesh.vertices.push_back({{ hw, 0.0f, -hd}, n, {uvRepeat, 0.0f},     vertexColor});
    mesh.vertices.push_back({{-hw, 0.0f, -hd}, n, {0.0f,     0.0f},     vertexColor});

    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.RecomputeBounds();
    return mesh;
}

MeshData MeshGenerator::GenerateTorus(float majorRadius, float minorRadius, int majorSegs, int minorSegs, const glm::vec3& vertexColor) {
    MeshData mesh;
    mesh.name = "Torus";
    majorSegs = std::max(majorSegs, 8);
    minorSegs = std::max(minorSegs, 6);

    for (int i = 0; i <= majorSegs; ++i) {
        float u = static_cast<float>(i) / static_cast<float>(majorSegs);
        float theta = u * 2.0f * kPi;
        float cosTheta = std::cos(theta);
        float sinTheta = std::sin(theta);

        for (int j = 0; j <= minorSegs; ++j) {
            float v = static_cast<float>(j) / static_cast<float>(minorSegs);
            float phi = v * 2.0f * kPi;
            float cosPhi = std::cos(phi);
            float sinPhi = std::sin(phi);

            float r = majorRadius + minorRadius * cosPhi;
            glm::vec3 pos(r * cosTheta, minorRadius * sinPhi, r * sinTheta);
            glm::vec3 center(majorRadius * cosTheta, 0.0f, majorRadius * sinTheta);
            glm::vec3 norm = glm::normalize(pos - center);

            mesh.vertices.push_back({pos, norm, {u * 2.0f, v}, vertexColor});
        }
    }

    int stride = minorSegs + 1;
    for (int i = 0; i < majorSegs; ++i) {
        for (int j = 0; j < minorSegs; ++j) {
            uint32_t a = static_cast<uint32_t>(i * stride + j);
            uint32_t b = static_cast<uint32_t>((i + 1) * stride + j);
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(a + 1);

            mesh.indices.push_back(a + 1);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b + 1);
        }
    }

    mesh.RecomputeBounds();
    return mesh;
}

MeshData MeshGenerator::GenerateWorldGridLines(int halfExtent, float step) {
    MeshData grid;
    grid.name = "WorldReferenceGrid";
    float maxCoord = static_cast<float>(halfExtent) * step;

    for (int i = -halfExtent; i <= halfExtent; ++i) {
        float coord = static_cast<float>(i) * step;
        bool isMajor = (i % 5 == 0);
        glm::vec3 col = isMajor ? glm::vec3(0.32f, 0.36f, 0.42f) : glm::vec3(0.18f, 0.20f, 0.24f);

        // X axis line (at Z = 0)
        glm::vec3 xCol = (i == 0) ? glm::vec3(0.88f, 0.25f, 0.25f) : col;
        uint32_t baseX = static_cast<uint32_t>(grid.vertices.size());
        grid.vertices.push_back({{-maxCoord, 0.002f, coord}, {0, 1, 0}, {0, 0}, xCol});
        grid.vertices.push_back({{ maxCoord, 0.002f, coord}, {0, 1, 0}, {1, 0}, xCol});
        grid.indices.push_back(baseX);
        grid.indices.push_back(baseX + 1);

        // Z axis line (at X = 0)
        glm::vec3 zCol = (i == 0) ? glm::vec3(0.22f, 0.55f, 0.95f) : col;
        uint32_t baseZ = static_cast<uint32_t>(grid.vertices.size());
        grid.vertices.push_back({{coord, 0.002f, -maxCoord}, {0, 1, 0}, {0, 0}, zCol});
        grid.vertices.push_back({{coord, 0.002f,  maxCoord}, {0, 1, 0}, {0, 1}, zCol});
        grid.indices.push_back(baseZ);
        grid.indices.push_back(baseZ + 1);
    }
    grid.RecomputeBounds();
    return grid;
}

MeshData MeshGenerator::ApplySubdivisionModifier(const MeshData& source) {
    MeshData out;
    out.name = source.name + "_Subdivided";
    if (source.indices.size() < 3 || source.indices.size() % 3 != 0) {
        return source;
    }

    auto midpoint = [](const Vertex3D& a, const Vertex3D& b) -> Vertex3D {
        Vertex3D m;
        m.position = (a.position + b.position) * 0.5f;
        glm::vec3 nSum = a.normal + b.normal;
        float len = glm::length(nSum);
        m.normal = (len > 1e-5f) ? (nSum / len) : a.normal;
        m.uv = (a.uv + b.uv) * 0.5f;
        m.color = (a.color + b.color) * 0.5f;
        return m;
    };

    for (size_t i = 0; i < source.indices.size(); i += 3) {
        const Vertex3D& v0 = source.vertices[source.indices[i]];
        const Vertex3D& v1 = source.vertices[source.indices[i + 1]];
        const Vertex3D& v2 = source.vertices[source.indices[i + 2]];

        Vertex3D m01 = midpoint(v0, v1);
        Vertex3D m12 = midpoint(v1, v2);
        Vertex3D m20 = midpoint(v2, v0);

        uint32_t base = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(v0);  // 0
        out.vertices.push_back(v1);  // 1
        out.vertices.push_back(v2);  // 2
        out.vertices.push_back(m01); // 3
        out.vertices.push_back(m12); // 4
        out.vertices.push_back(m20); // 5

        // 4 sub-triangles
        out.indices.insert(out.indices.end(), {
            base + 0, base + 3, base + 5,
            base + 3, base + 1, base + 4,
            base + 5, base + 4, base + 2,
            base + 3, base + 4, base + 5
        });
    }
    out.RecomputeBounds();
    return out;
}

} // namespace platz::creative
