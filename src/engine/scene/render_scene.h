#pragma once

#include "core/project_state.h"
#include "creative/mesh_generator.h"

#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace platz::engine::scene {

enum class GizmoMode {
    Translate,
    Rotate,
    Scale
};

struct SnapSettings {
    bool enableSnap{true};
    float translateSnapStep{0.5f};
    float rotateSnapDeg{15.0f};
    float scaleSnapStep{0.1f};
};

struct WorldRay {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

struct PickResult {
    bool hit{false};
    core::ObjectId objectId{0};
    float distance{0.0f};
    glm::vec3 worldHitPoint{0.0f};
};

class ViewportCamera {
public:
    ViewportCamera();

    void Orbit(float deltaYawDeg, float deltaPitchDeg);
    void Pan(float deltaRight, float deltaUp);
    void Zoom(float deltaDistance);
    void FocusOnPoint(const glm::vec3& targetPoint, float distance = 8.0f);

    glm::vec3 GetEyePosition() const;
    glm::vec3 GetForwardDirection() const;
    glm::vec3 GetRightDirection() const;
    glm::vec3 GetUpDirection() const;

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspectRatio) const;
    glm::mat4 GetViewProjectionMatrix(float aspectRatio) const;

    WorldRay ScreenPointToWorldRay(
        float pixelX,
        float pixelY,
        float viewportWidth,
        float viewportHeight) const;

    bool IsOrthographic() const { return orthographic_; }
    void SetOrthographic(bool ortho) { orthographic_ = ortho; }

    float GetFovYDeg() const { return fovYDeg_; }
    void SetFovYDeg(float fov) { fovYDeg_ = fov; }

    float GetExposure() const { return exposure_; }
    void SetExposure(float exp) { exposure_ = exp; }

    const glm::vec3& GetTarget() const { return target_; }
    float GetDistance() const { return distance_; }
    float GetYawDeg() const { return yawDeg_; }
    float GetPitchDeg() const { return pitchDeg_; }

private:
    glm::vec3 target_{0.0f, 1.0f, 0.0f};
    float distance_{11.5f};
    float yawDeg_{-38.0f};
    float pitchDeg_{24.0f};
    float fovYDeg_{50.0f};
    float nearClip_{0.1f};
    float farClip_{500.0f};
    float exposure_{1.15f};
    bool orthographic_{false};
};

class ScenePickerAndGizmo {
public:
    // Analytical 3D ray picking against canonical or runtime objects (PLZ-RENDER-001, N-01)
    static PickResult PickObjectByRay(
        const std::vector<core::SceneObject>& objects,
        const WorldRay& ray);

    // Apply transform delta with optional grid/angle/scale snapping (Gate 2)
    static core::TransformComponent ApplyGizmoDelta(
        const core::TransformComponent& currentTransform,
        GizmoMode mode,
        const glm::vec3& rawDelta,
        const SnapSettings& snap);

    static float SnapValue(float value, float step);
    static glm::vec3 SnapVec3(const glm::vec3& v, float step);
};

} // namespace platz::engine::scene
