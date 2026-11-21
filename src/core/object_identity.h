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

struct MeshComponent {
    std::string primitiveType{"cube"}; // "cube", "sphere", "cylinder", "plane", "torus", "custom_asset"
    AssetId meshAssetId{0};
    bool castShadows{true};
    bool visible{true};
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

struct BehaviorRule {
    std::string ruleId{"rule_1"};
    std::string triggerType{"OnTick"};     // "OnTick", "OnStart", "OnProximity", "OnInteract", "OnResourceTransfer"
    std::string conditionExpr{"always"};   // "always", "state == 'Idle'", "state == 'Active'", "distance < 4.0", "inventory >= amount"
    std::string actionType{"RotateY"};     // "RotateY", "BobVertical", "OrbitOrigin", "TransitionState", "AddInventory", "TransferInventory", "PulseEmissive"
    float paramA{45.0f};                   // e.g. speed deg/s or amplitude or proximity radius
    float paramB{1.5f};                    // e.g. frequency
    std::string requiredState{};
    std::string targetState{};
    std::string resourceName{"energy_crystal"};
    int64_t resourceAmount{1};
    ObjectId targetObjectId{0};
};

struct LogicComponent {
    std::string currentState{"Idle"};
    std::string stationCategory{"General"}; // Used for living validation world stations (PLZ-TEST-001..008)
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
    LogicComponent logic;

    nlohmann::json ToJson() const;
    static SceneObject FromJson(const nlohmann::json& j);
};

} // namespace platz::core
