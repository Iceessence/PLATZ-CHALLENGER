#include "core/object_identity.h"

#include <atomic>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace platz::core {

namespace {
std::atomic<uint64_t> gNextObjectCounter{1000};
std::atomic<uint64_t> gNextAssetCounter{5000};
} // namespace

ObjectId GenerateObjectId() {
    return gNextObjectCounter.fetch_add(1, std::memory_order_relaxed);
}

AssetId GenerateAssetId() {
    return gNextAssetCounter.fetch_add(1, std::memory_order_relaxed);
}

std::string FormatObjectId(ObjectId id) {
    std::ostringstream oss;
    oss << "OBJ-" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << id;
    return oss.str();
}

std::string FormatAssetId(AssetId id) {
    std::ostringstream oss;
    oss << "AST-" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << id;
    return oss.str();
}

static bool Vec3Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool TransformComponent::IsFinite() const {
    if (!Vec3Finite(position) || !Vec3Finite(rotationEulerDeg) || !Vec3Finite(scale)) {
        return false;
    }
    if (std::abs(scale.x) < 1e-5f || std::abs(scale.y) < 1e-5f || std::abs(scale.z) < 1e-5f) {
        return false;
    }
    if ( std::abs(position.x) > 1e6f || std::abs(position.y) > 1e6f || std::abs(position.z) > 1e6f) {
        return false;
    }
    return true;
}

void TransformComponent::Sanitize() {
    auto fixCoord = [](float v, float fallback, float maxAbs) {
        if (!std::isfinite(v)) return fallback;
        if (v > maxAbs) return maxAbs;
        if (v < -maxAbs) return -maxAbs;
        return v;
    };
    position.x = fixCoord(position.x, 0.0f, 100000.0f);
    position.y = fixCoord(position.y, 0.0f, 100000.0f);
    position.z = fixCoord(position.z, 0.0f, 100000.0f);

    rotationEulerDeg.x = fixCoord(rotationEulerDeg.x, 0.0f, 36000.0f);
    rotationEulerDeg.y = fixCoord(rotationEulerDeg.y, 0.0f, 36000.0f);
    rotationEulerDeg.z = fixCoord(rotationEulerDeg.z, 0.0f, 36000.0f);

    auto fixScale = [](float s) {
        if (!std::isfinite(s) || std::abs(s) < 0.001f) return 1.0f;
        if (s > 10000.0f) return 10000.0f;
        if (s < -10000.0f) return -10000.0f;
        return s;
    };
    scale.x = fixScale(scale.x);
    scale.y = fixScale(scale.y);
    scale.z = fixScale(scale.z);
}

glm::mat4 TransformComponent::ToLocalMatrix() const {
    TransformComponent safe = *this;
    if (!safe.IsFinite()) {
        safe.Sanitize();
    }
    glm::mat4 m(1.0f);
    m = glm::translate(m, safe.position);
    m = glm::rotate(m, glm::radians(safe.rotationEulerDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, glm::radians(safe.rotationEulerDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, glm::radians(safe.rotationEulerDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::scale(m, safe.scale);
    return m;
}

static nlohmann::json Vec3ToJson(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

static glm::vec3 JsonToVec3(const nlohmann::json& j, const glm::vec3& defVal = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return defVal;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

static nlohmann::json Vec4ToJson(const glm::vec4& v) {
    return nlohmann::json::array({v.x, v.y, v.z, v.w});
}

static glm::vec4 JsonToVec4(const nlohmann::json& j, const glm::vec4& defVal = glm::vec4(1.0f)) {
    if (!j.is_array() || j.size() < 4) return defVal;
    return glm::vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
}

nlohmann::json SceneObject::ToJson() const {
    nlohmann::json rulesJson = nlohmann::json::array();
    for (const auto& r : logic.rules) {
        rulesJson.push_back({
            {"ruleId", r.ruleId},
            {"triggerType", r.triggerType},
            {"conditionExpr", r.conditionExpr},
            {"actionType", r.actionType},
            {"paramA", r.paramA},
            {"paramB", r.paramB},
            {"requiredState", r.requiredState},
            {"targetState", r.targetState},
            {"resourceName", r.resourceName},
            {"resourceAmount", r.resourceAmount},
            {"targetObjectId", r.targetObjectId}
        });
    }

    return {
        {"id", id},
        {"name", name},
        {"parentId", parentId},
        {"layer", layer},
        {"visible", visible},
        {"locked", locked},
        {"transform", {
            {"position", Vec3ToJson(transform.position)},
            {"rotationEulerDeg", Vec3ToJson(transform.rotationEulerDeg)},
            {"scale", Vec3ToJson(transform.scale)}
        }},
        {"mesh", {
            {"primitiveType", mesh.primitiveType},
            {"meshAssetId", mesh.meshAssetId},
            {"castShadows", mesh.castShadows},
            {"visible", mesh.visible}
        }},
        {"material", {
            {"materialName", material.materialName},
            {"baseColor", Vec4ToJson(material.baseColor)},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"emissiveColor", Vec3ToJson(material.emissiveColor)},
            {"emissiveStrength", material.emissiveStrength},
            {"normalScale", material.normalScale},
            {"useProceduralChecker", material.useProceduralChecker},
            {"albedoTextureAssetId", material.albedoTextureAssetId}
        }},
        {"light", {
            {"enabled", light.enabled},
            {"lightType", light.lightType},
            {"color", Vec3ToJson(light.color)},
            {"intensity", light.intensity},
            {"range", light.range}
        }},
        {"physics", {
            {"enabled", physics.enabled},
            {"isDynamic", physics.isDynamic},
            {"isTrigger", physics.isTrigger},
            {"mass", physics.mass},
            {"restitution", physics.restitution},
            {"friction", physics.friction},
            {"linearVelocity", Vec3ToJson(physics.linearVelocity)},
            {"angularVelocityDeg", Vec3ToJson(physics.angularVelocityDeg)},
            {"colliderHalfExtents", Vec3ToJson(physics.colliderHalfExtents)}
        }},
        {"logic", {
            {"currentState", logic.currentState},
            {"stationCategory", logic.stationCategory},
            {"stationDescription", logic.stationDescription},
            {"inventory", logic.inventory},
            {"variables", logic.variables},
            {"rules", rulesJson}
        }}
    };
}

SceneObject SceneObject::FromJson(const nlohmann::json& j) {
    SceneObject obj;
    obj.id = j.value("id", ObjectId{0});
    obj.name = j.value("name", std::string("SceneObject"));
    obj.parentId = j.value("parentId", ObjectId{0});
    obj.layer = j.value("layer", std::string("Default"));
    obj.visible = j.value("visible", true);
    obj.locked = j.value("locked", false);

    if (j.contains("transform")) {
        const auto& t = j["transform"];
        obj.transform.position = JsonToVec3(t.value("position", nlohmann::json::array()), glm::vec3(0.0f));
        obj.transform.rotationEulerDeg = JsonToVec3(t.value("rotationEulerDeg", nlohmann::json::array()), glm::vec3(0.0f));
        obj.transform.scale = JsonToVec3(t.value("scale", nlohmann::json::array()), glm::vec3(1.0f));
    }
    if (j.contains("mesh")) {
        const auto& m = j["mesh"];
        obj.mesh.primitiveType = m.value("primitiveType", std::string("cube"));
        obj.mesh.meshAssetId = m.value("meshAssetId", AssetId{0});
        obj.mesh.castShadows = m.value("castShadows", true);
        obj.mesh.visible = m.value("visible", true);
    }
    if (j.contains("material")) {
        const auto& mat = j["material"];
        obj.material.materialName = mat.value("materialName", std::string("StandardPBR"));
        obj.material.baseColor = JsonToVec4(mat.value("baseColor", nlohmann::json::array()), glm::vec4(0.8f, 0.8f, 0.85f, 1.0f));
        obj.material.metallic = mat.value("metallic", 0.15f);
        obj.material.roughness = mat.value("roughness", 0.42f);
        obj.material.emissiveColor = JsonToVec3(mat.value("emissiveColor", nlohmann::json::array()), glm::vec3(0.0f));
        obj.material.emissiveStrength = mat.value("emissiveStrength", 0.0f);
        obj.material.normalScale = mat.value("normalScale", 1.0f);
        obj.material.useProceduralChecker = mat.value("useProceduralChecker", false);
        obj.material.albedoTextureAssetId = mat.value("albedoTextureAssetId", AssetId{0});
    }
    if (j.contains("light")) {
        const auto& l = j["light"];
        obj.light.enabled = l.value("enabled", false);
        obj.light.lightType = l.value("lightType", std::string("point"));
        obj.light.color = JsonToVec3(l.value("color", nlohmann::json::array()), glm::vec3(1.0f));
        obj.light.intensity = l.value("intensity", 3.5f);
        obj.light.range = l.value("range", 24.0f);
    }
    if (j.contains("physics")) {
        const auto& p = j["physics"];
        obj.physics.enabled = p.value("enabled", false);
        obj.physics.isDynamic = p.value("isDynamic", false);
        obj.physics.isTrigger = p.value("isTrigger", false);
        obj.physics.mass = p.value("mass", 1.0f);
        obj.physics.restitution = p.value("restitution", 0.35f);
        obj.physics.friction = p.value("friction", 0.5f);
        obj.physics.linearVelocity = JsonToVec3(p.value("linearVelocity", nlohmann::json::array()), glm::vec3(0.0f));
        obj.physics.angularVelocityDeg = JsonToVec3(p.value("angularVelocityDeg", nlohmann::json::array()), glm::vec3(0.0f));
        obj.physics.colliderHalfExtents = JsonToVec3(p.value("colliderHalfExtents", nlohmann::json::array()), glm::vec3(0.5f));
    }
    if (j.contains("logic")) {
        const auto& lg = j["logic"];
        obj.logic.currentState = lg.value("currentState", std::string("Idle"));
        obj.logic.stationCategory = lg.value("stationCategory", std::string("General"));
        obj.logic.stationDescription = lg.value("stationDescription", std::string(""));
        if (lg.contains("inventory") && lg["inventory"].is_object()) {
            obj.logic.inventory = lg["inventory"].get<std::map<std::string, int64_t>>();
        }
        if (lg.contains("variables") && lg["variables"].is_object()) {
            obj.logic.variables = lg["variables"].get<std::map<std::string, double>>();
        }
        if (lg.contains("rules") && lg["rules"].is_array()) {
            for (const auto& rj : lg["rules"]) {
                BehaviorRule r;
                r.ruleId = rj.value("ruleId", std::string("rule_1"));
                r.triggerType = rj.value("triggerType", std::string("OnTick"));
                r.conditionExpr = rj.value("conditionExpr", std::string("always"));
                r.actionType = rj.value("actionType", std::string("RotateY"));
                r.paramA = rj.value("paramA", 45.0f);
                r.paramB = rj.value("paramB", 1.5f);
                r.requiredState = rj.value("requiredState", std::string(""));
                r.targetState = rj.value("targetState", std::string(""));
                r.resourceName = rj.value("resourceName", std::string("energy_crystal"));
                r.resourceAmount = rj.value("resourceAmount", int64_t{1});
                r.targetObjectId = rj.value("targetObjectId", ObjectId{0});
                obj.logic.rules.push_back(std::move(r));
            }
        }
    }

    if (obj.id >= gNextObjectCounter.load(std::memory_order_relaxed)) {
        gNextObjectCounter.store(obj.id + 1, std::memory_order_relaxed);
    }
    return obj;
}

} // namespace platz::core
