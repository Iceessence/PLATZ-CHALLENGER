#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <json.hpp>

namespace platz::core {

using ObjectId = uint64_t;
using AssetId = uint64_t;
using RevisionId = uint64_t;

ObjectId GenerateObjectId();
AssetId GenerateAssetId();
std::string FormatObjectId(ObjectId id);
std::string FormatAssetId(AssetId id);

struct TransformComponent {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 rotationEulerDeg{0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f, 1.0f, 1.0f};

    bool IsFinite() const;
    void Sanitize(); // Clamps/resets NaN or Inf values safely (N-10)
    glm::mat4 ToLocalMatrix() const;
};

// CPU-evaluated non-destructive parametric 3D mesh modifier uploaded to Vulkan VMA device memory (PLZ-MODEL-004, PLZ-MODEL-005)
struct MeshModifier {
    std::string modifierId{"mod_1"};
    std::string modifierType{"Subdivide"}; // "Subdivide", "BevelChamfer", "TaperTwist", "MirrorX", "VertexNoiseDisplace"
    bool enabled{true};
    float paramA{0.25f};
    float paramB{1.0f};
    uint32_t seed{2026u};
};

struct MeshComponent {
    std::string primitiveType{"cube"}; // "cube", "sphere", "cylinder", "plane", "torus", "custom_asset"
    AssetId meshAssetId{0};
    bool castShadows{true};
    bool visible{true};
    std::vector<MeshModifier> modifierStack;
};

struct MaterialComponent {
    std::string materialName{"StandardPBR"};
    glm::vec4 baseColor{0.78f, 0.80f, 0.84f, 1.0f};
    float metallic{0.15f};
    float roughness{0.42f};
    glm::vec3 emissiveColor{0.0f, 0.0f, 0.0f};
    float emissiveStrength{0.0f};
    float normalScale{1.0f};
    bool useProceduralChecker{false};
    AssetId albedoTextureAssetId{0};
    AssetId pixelSourceAssetId{0};      // Editable multi-layer PixelArtCanvas source blob ID (PLZ-ART-001, PLZ-ART-007)
    std::string materialGraphJson{};    // Serialized Visual Material DAG Graph (PLZ-MAT-001, PLZ-RENDERX-002)
    std::string pixelCanvasJson{};      // Inline editable multi-layer PixelArtCanvas JSON for atomic Undo/Redo & reopen
};

struct LightComponent {
    bool enabled{false};
    std::string lightType{"point"}; // "directional", "point"
    glm::vec3 color{1.0f, 0.96f, 0.90f};
    float intensity{3.5f};
    float range{24.0f};
};

struct PhysicsComponent {
    bool enabled{false};
    bool isDynamic{false};
    bool isTrigger{false};
    float mass{1.0f};
    float restitution{0.35f};
    float friction{0.5f};
    glm::vec3 linearVelocity{0.0f, 0.0f, 0.0f};
    glm::vec3 angularVelocityDeg{0.0f, 0.0f, 0.0f};
    glm::vec3 colliderHalfExtents{0.5f, 0.5f, 0.5f};
};

// Keyframed Animation Timeline Track & Component (PLZ-MEDIA-001, PLZ-MEDIA-002, PLZ-CREATEX-006)
struct AnimationKeyframe {
    float timeSec{0.0f};
    glm::vec3 value{0.0f};
    std::string interpolation{"EaseInOut"}; // "Linear", "EaseInOut", "Step"
};

struct AnimationTrack {
    std::string trackId{"track_pos"};
    std::string targetProperty{"position"}; // "position", "rotationEulerDeg", "scale", "emissiveStrength"
    std::vector<AnimationKeyframe> keyframes;
};

struct AnimationComponent {
    bool enabled{false};
    bool loop{true};
    float durationSec{4.0f};
    std::vector<AnimationTrack> tracks;
};

// Spatial Audio & Procedural DSP Source Component (PLZ-MEDIA-008, PLZ-MEDIA-009, PLZ-CREATEX-007)
struct AudioSourceComponent {
    bool enabled{false};
    std::string cueType{"CrystalChime"}; // "CrystalChime", "PhysicsImpact", "TransferArp", "DronePulse", "UiClick"
    float volume{0.75f};
    float pitch{1.0f};
    float maxDistance{25.0f};
    bool spatial3D{true};
    bool playOnInteract{true};
    bool playOnCollision{true};
    float lowPassCutoffHz{12000.0f};
};

struct BehaviorRule {
    std::string ruleId{"rule_1"};
    std::string triggerType{"OnTick"};     // "OnTick", "OnStart", "OnProximity", "OnInteract", "OnResourceTransfer"
    std::string conditionExpr{"always"};   // "always", "state == 'Idle'", "state == 'Active'", "distance < 4.0", "inventory >= amount"
    std::string actionType{"RotateY"};     // "RotateY", "BobVertical", "OrbitOrigin", "PatrolWaypoints", "TransitionState", "AddInventory", "TransferInventory", "PulseEmissive", "ModifyVariable", "PlayAudioCue"
    float paramA{45.0f};
    float paramB{1.5f};
    std::string requiredState{};
    std::string targetState{};
    std::string resourceName{"energy_crystal"};
    int64_t resourceAmount{1};
    ObjectId targetObjectId{0};
};

struct LogicComponent {
    std::string currentState{"Idle"};
    std::string stationCategory{"General"};
    std::string stationDescription{};
    std::map<std::string, int64_t> inventory;
    std::map<std::string, double> variables;
    std::vector<BehaviorRule> rules;
};

struct SceneObject {
    ObjectId id{0};
    std::string name{"SceneObject"};
    ObjectId parentId{0};
    std::string layer{"Default"};
    bool visible{true};
    bool locked{false};

    TransformComponent transform;
    MeshComponent mesh;
    MaterialComponent material;
    LightComponent light;
    PhysicsComponent physics;
    AnimationComponent animation;
    AudioSourceComponent audio;
    LogicComponent logic;

    nlohmann::json ToJson() const;
    static SceneObject FromJson(const nlohmann::json& j);
};

} // namespace platz::core
