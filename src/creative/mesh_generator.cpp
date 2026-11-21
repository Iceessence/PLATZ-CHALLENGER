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
    glm::vec3 span = bmax - bmin;
    if (span.x < 0.04f) { bmin.x -= 0.02f; bmax.x += 0.02f; }
    if (span.y < 0.04f) { bmin.y -= 0.02f; bmax.y += 0.02f; }
    if (span.z < 0.04f) { bmin.z -= 0.02f; bmax.z += 0.02f; }
    localBounds.minBounds = bmin;
    localBounds.maxBounds = bmax;
}

void MeshData::RecomputeSmoothNormals() {
    if (vertices.empty() || indices.size() < 3 || (indices.size() % 3) != 0) return;
    std::vector<glm::vec3> accum(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
        glm::vec3 e1 = vertices[i1].position - vertices[i0].position;
        glm::vec3 e2 = vertices[i2].position - vertices[i0].position;
        glm::vec3 fn = glm::cross(e1, e2);
        accum[i0] += fn;
        accum[i1] += fn;
        accum[i2] += fn;
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
        float len = glm::length(accum[v]);
        if (len > 1e-6f) {
            vertices[v].normal = accum[v] / len;
        }
    }
}

MeshValidationReport MeshData::AnalyzeMeshQuality() const {
    MeshValidationReport rep{};
    rep.vertexCount = static_cast<uint32_t>(vertices.size());
    rep.triangleCount = static_cast<uint32_t>(indices.size() / 3);
    rep.dimensions = localBounds.maxBounds - localBounds.minBounds;

    if (vertices.empty() || indices.empty()) {
        rep.diagnostic = "Mesh has zero vertices or indices";
        return rep;
    }
    if (indices.size() % 3 != 0) {
        rep.diagnostic = "Index count is not divisible by 3";
        return rep;
    }

    uint32_t vCount = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vCount) {
            ++rep.outOfBoundsIndexCount;
        }
    }
    for (const auto& v : vertices) {
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z) ||
            !std::isfinite(v.normal.x) || !std::isfinite(v.normal.y) || !std::isfinite(v.normal.z)) {
            ++rep.nonFiniteVertexCount;
        }
    }
    if (rep.outOfBoundsIndexCount == 0) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3& p0 = vertices[indices[i]].position;
            const glm::vec3& p1 = vertices[indices[i + 1]].position;
            const glm::vec3& p2 = vertices[indices[i + 2]].position;
            float area2 = glm::length(glm::cross(p1 - p0, p2 - p0));
            if (area2 < 1e-7f) {
                ++rep.degenerateTriangleCount;
            }
        }
    }

    if (rep.outOfBoundsIndexCount > 0) {
        rep.diagnostic = "Mesh has " + std::to_string(rep.outOfBoundsIndexCount) + " out-of-bounds indices";
        return rep;
    }
    if (rep.nonFiniteVertexCount > 0) {
        rep.diagnostic = "Mesh has " + std::to_string(rep.nonFiniteVertexCount) + " non-finite vertices";
        return rep;
    }

    rep.valid = true;
    rep.diagnostic = "Valid topology: " + std::to_string(rep.vertexCount) + " verts, " +
                     std::to_string(rep.triangleCount) + " tris (" +
                     std::to_string(rep.degenerateTriangleCount) + " degenerate)";
    return rep;
}

bool MeshData::ValidateTopology(std::string* outDiagnostic) const {
    auto rep = AnalyzeMeshQuality();
    if (outDiagnostic) *outDiagnostic = rep.diagnostic;
    return rep.valid;
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
        {{ 0,  0,  1}, { 1, 0, 0}, {0, 1, 0}},
        {{ 0,  0, -1}, {-1, 0, 0}, {0, 1, 0}},
        {{ 1,  0,  0}, { 0, 0,-1}, {0, 1, 0}},
        {{-1,  0,  0}, { 0, 0, 1}, {0, 1, 0}},
        {{ 0,  1,  0}, { 1, 0, 0}, {0, 0,-1}},
        {{ 0, -1,  0}, { 1, 0, 0}, {0, 0, 1}}
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

        glm::vec3 xCol = (i == 0) ? glm::vec3(0.88f, 0.25f, 0.25f) : col;
        uint32_t baseX = static_cast<uint32_t>(grid.vertices.size());
        grid.vertices.push_back({{-maxCoord, 0.002f, coord}, {0, 1, 0}, {0, 0}, xCol});
        grid.vertices.push_back({{ maxCoord, 0.002f, coord}, {0, 1, 0}, {1, 0}, xCol});
        grid.indices.push_back(baseX);
        grid.indices.push_back(baseX + 1);

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
    out.name = source.name + "_Subdiv";
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
        out.vertices.push_back(v0);
        out.vertices.push_back(v1);
        out.vertices.push_back(v2);
        out.vertices.push_back(m01);
        out.vertices.push_back(m12);
        out.vertices.push_back(m20);

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

MeshData MeshGenerator::ApplyBevelChamferModifier(const MeshData& source, float chamferAmount) {
    MeshData out = source;
    out.name = source.name + "_Bevel";
    float amt = std::clamp(chamferAmount, -0.35f, 0.35f);
    glm::vec3 center = (source.localBounds.minBounds + source.localBounds.maxBounds) * 0.5f;

    for (auto& v : out.vertices) {
        glm::vec3 radial = v.position - center;
        float rLen = glm::length(radial);
        glm::vec3 rDir = (rLen > 1e-5f) ? (radial / rLen) : v.normal;
        // Spherize/chamfer sharp corners smoothly toward radial normal envelope
        v.position = glm::mix(v.position, center + rDir * rLen, amt);
        v.normal = glm::normalize(glm::mix(v.normal, rDir, std::abs(amt) * 0.65f));
    }
    out.RecomputeBounds();
    return out;
}

MeshData MeshGenerator::ApplyTaperTwistModifier(const MeshData& source, float twistAngleDeg, float taperFactor) {
    MeshData out = source;
    out.name = source.name + "_TaperTwist";
    float minY = source.localBounds.minBounds.y;
    float maxY = source.localBounds.maxBounds.y;
    float spanY = std::max(maxY - minY, 0.01f);
    float twistRadTotal = glm::radians(twistAngleDeg);
    float safeTaper = std::clamp(taperFactor, 0.15f, 2.5f);

    for (auto& v : out.vertices) {
        float t = std::clamp((v.position.y - minY) / spanY, 0.0f, 1.0f);
        float profileScale = glm::mix(1.0f, safeTaper, t);
        float angle = (t - 0.5f) * twistRadTotal;
        float c = std::cos(angle);
        float s = std::sin(angle);

        float x = v.position.x * profileScale;
        float z = v.position.z * profileScale;
        v.position.x = x * c - z * s;
        v.position.z = x * s + z * c;

        float nx = v.normal.x;
        float nz = v.normal.z;
        v.normal.x = nx * c - nz * s;
        v.normal.z = nx * s + nz * c;
        float nLen = glm::length(v.normal);
        if (nLen > 1e-5f) v.normal /= nLen;
    }
    out.RecomputeBounds();
    return out;
}

MeshData MeshGenerator::ApplyMirrorXModifier(const MeshData& source, float mirrorOffset) {
    MeshData out = source;
    out.name = source.name + "_MirrorX";
    float off = std::abs(mirrorOffset);

    // Shift original to +X side
    for (auto& v : out.vertices) {
        v.position.x += off;
    }

    uint32_t baseCount = static_cast<uint32_t>(source.vertices.size());
    size_t origIdxCount = source.indices.size();

    for (const auto& v : source.vertices) {
        Vertex3D mv = v;
        mv.position.x = -(v.position.x + off);
        mv.normal.x = -v.normal.x;
        out.vertices.push_back(mv);
    }

    // Reverse triangle winding on mirrored half to preserve outward normals
    for (size_t i = 0; i + 2 < origIdxCount; i += 3) {
        out.indices.push_back(baseCount + source.indices[i]);
        out.indices.push_back(baseCount + source.indices[i + 2]);
        out.indices.push_back(baseCount + source.indices[i + 1]);
    }

    out.RecomputeBounds();
    return out;
}

MeshData MeshGenerator::ApplyVertexNoiseDisplaceModifier(
    const MeshData& source,
    float amplitude,
    float frequency,
    uint32_t seed) {
    MeshData out = source;
    out.name = source.name + "_Displaced";
    float phase = static_cast<float>(seed % 1024u) * 0.137f;

    for (auto& v : out.vertices) {
        float n = std::sin(v.position.x * frequency + phase) *
                  std::cos(v.position.y * frequency * 1.13f - phase) *
                  std::sin(v.position.z * frequency * 0.89f + phase * 0.5f);
        v.position += v.normal * (n * amplitude);
    }
    out.RecomputeSmoothNormals();
    out.RecomputeBounds();
    return out;
}

MeshData MeshGenerator::EvaluateModifierStack(
    const MeshData& baseMesh,
    const std::vector<core::MeshModifier>& stack) {
    MeshData current = baseMesh;
    int subdivCount = 0;
    for (const auto& mod : stack) {
        if (!mod.enabled) continue;
        if (mod.modifierType == "Subdivide") {
            // Cap recursive subdivisions at 3 levels to protect real-time budget
            if (subdivCount < 3) {
                current = ApplySubdivisionModifier(current);
                ++subdivCount;
            }
        } else if (mod.modifierType == "BevelChamfer") {
            current = ApplyBevelChamferModifier(current, mod.paramA);
        } else if (mod.modifierType == "TaperTwist") {
            current = ApplyTaperTwistModifier(current, mod.paramA, mod.paramB);
        } else if (mod.modifierType == "MirrorX") {
            current = ApplyMirrorXModifier(current, mod.paramA);
        } else if (mod.modifierType == "VertexNoiseDisplace") {
            current = ApplyVertexNoiseDisplaceModifier(current, mod.paramA, mod.paramB, mod.seed);
        }
    }
    current.RecomputeBounds();
    return current;
}

} // namespace platz::creative
