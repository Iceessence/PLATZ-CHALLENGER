#pragma once

#include "core/object_identity.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace platz::creative {

// Matches shaders/pbr_surface.slang and shaders/grid_sky.slang VSInput layout
struct Vertex3D {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec3 color{1.0f};
};

struct Aabb3D {
    glm::vec3 minBounds{-0.5f, -0.5f, -0.5f};
    glm::vec3 maxBounds{0.5f, 0.5f, 0.5f};

    bool IntersectsRay(
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDirNormalized,
        float* outDistance) const;
};

struct MeshValidationReport {
    bool valid{false};
    uint32_t vertexCount{0};
    uint32_t triangleCount{0};
    uint32_t degenerateTriangleCount{0};
    uint32_t outOfBoundsIndexCount{0};
    uint32_t nonFiniteVertexCount{0};
    glm::vec3 dimensions{0.0f};
    std::string diagnostic;
};

struct MeshData {
    std::string name{"Mesh"};
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
    Aabb3D localBounds;

    void RecomputeBounds();
    void RecomputeSmoothNormals();
    bool ValidateTopology(std::string* outDiagnostic = nullptr) const;
    MeshValidationReport AnalyzeMeshQuality() const;

    // Binary format serialization for PLATZ content-addressed asset store (.blob)
    std::vector<uint8_t> SerializeToBinaryBlob() const;
    static bool DeserializeFromBinaryBlob(
        std::span<const uint8_t> blob,
        MeshData* outMesh,
        std::string* outError = nullptr);
};

class MeshGenerator {
public:
    static MeshData GenerateCube(float size = 1.0f, const glm::vec3& vertexColor = glm::vec3(1.0f));
    static MeshData GenerateSphere(float radius = 0.6f, int slices = 28, int stacks = 18, const glm::vec3& vertexColor = glm::vec3(1.0f));
    static MeshData GenerateCylinder(float radius = 0.5f, float height = 1.2f, int slices = 24, const glm::vec3& vertexColor = glm::vec3(1.0f));
    static MeshData GeneratePlane(float width = 10.0f, float depth = 10.0f, float uvRepeat = 4.0f, const glm::vec3& vertexColor = glm::vec3(1.0f));
    static MeshData GenerateTorus(float majorRadius = 0.65f, float minorRadius = 0.22f, int majorSegs = 32, int minorSegs = 16, const glm::vec3& vertexColor = glm::vec3(1.0f));

    // Line-list reference grid for 3D Studio viewport
    static MeshData GenerateWorldGridLines(int halfExtent = 40, float step = 1.0f);

    // Non-destructive parametric modifiers & ordered stack evaluator (PLZ-MODEL-001, PLZ-MODEL-004, PLZ-MODEL-005)
    static MeshData ApplySubdivisionModifier(const MeshData& source);
    static MeshData ApplyBevelChamferModifier(const MeshData& source, float chamferAmount = 0.12f);
    static MeshData ApplyTaperTwistModifier(const MeshData& source, float twistAngleDeg = 45.0f, float taperFactor = 0.65f);
    static MeshData ApplyMirrorXModifier(const MeshData& source, float mirrorOffset = 0.75f);
    static MeshData ApplyVertexNoiseDisplaceModifier(const MeshData& source, float amplitude = 0.08f, float frequency = 3.0f, uint32_t seed = 2026u);

    static MeshData EvaluateModifierStack(
        const MeshData& baseMesh,
        const std::vector<core::MeshModifier>& stack);
};

} // namespace platz::creative
