#include "engine/scene/render_scene.h"

#include <algorithm>
#include <cmath>

namespace platz::engine::scene {

ViewportCamera::ViewportCamera() = default;

void ViewportCamera::Orbit(float deltaYawDeg, float deltaPitchDeg) {
    yawDeg_ += deltaYawDeg;
    pitchDeg_ = std::clamp(pitchDeg_ + deltaPitchDeg, -85.0f, 85.0f);
}

void ViewportCamera::Pan(float deltaRight, float deltaUp) {
    glm::vec3 right = GetRightDirection();
    glm::vec3 up = GetUpDirection();
    target_ += right * deltaRight + up * deltaUp;
}

void ViewportCamera::Zoom(float deltaDistance) {
    distance_ = std::clamp(distance_ + deltaDistance, 1.0f, 250.0f);
}

void ViewportCamera::FocusOnPoint(const glm::vec3& targetPoint, float distance) {
    target_ = targetPoint;
    distance_ = std::clamp(distance, 1.5f, 250.0f);
}

glm::vec3 ViewportCamera::GetEyePosition() const {
    float yawRad = glm::radians(yawDeg_);
    float pitchRad = glm::radians(pitchDeg_);
    glm::vec3 offset(
        distance_ * std::cos(pitchRad) * std::sin(yawRad),
        distance_ * std::sin(pitchRad),
        distance_ * std::cos(pitchRad) * std::cos(yawRad));
    return target_ + offset;
}

glm::vec3 ViewportCamera::GetForwardDirection() const {
    return glm::normalize(target_ - GetEyePosition());
}

glm::vec3 ViewportCamera::GetRightDirection() const {
    glm::vec3 fwd = GetForwardDirection();
    return glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 ViewportCamera::GetUpDirection() const {
    glm::vec3 right = GetRightDirection();
    glm::vec3 fwd = GetForwardDirection();
    return glm::normalize(glm::cross(right, fwd));
}

glm::mat4 ViewportCamera::GetViewMatrix() const {
    return glm::lookAt(GetEyePosition(), target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 ViewportCamera::GetProjectionMatrix(float aspectRatio) const {
    float safeAspect = (aspectRatio > 0.05f && std::isfinite(aspectRatio)) ? aspectRatio : (16.0f / 9.0f);
    glm::mat4 proj;
    if (orthographic_) {
        float halfH = distance_ * 0.5f;
        float halfW = halfH * safeAspect;
        proj = glm::orthoRH_ZO(-halfW, halfW, -halfH, halfH, nearClip_, farClip_);
    } else {
        proj = glm::perspectiveRH_ZO(glm::radians(fovYDeg_), safeAspect, nearClip_, farClip_);
    }
    // Vulkan clip space has inverted Y compared to OpenGL
    proj[1][1] *= -1.0f;
    return proj;
}

glm::mat4 ViewportCamera::GetViewProjectionMatrix(float aspectRatio) const {
    return GetProjectionMatrix(aspectRatio) * GetViewMatrix();
}

WorldRay ViewportCamera::ScreenPointToWorldRay(
    float pixelX,
    float pixelY,
    float viewportWidth,
    float viewportHeight) const {
    WorldRay ray{};
    if (viewportWidth <= 1.0f || viewportHeight <= 1.0f) {
        ray.origin = GetEyePosition();
        ray.direction = GetForwardDirection();
        return ray;
    }

    float ndcX = (2.0f * pixelX) / viewportWidth - 1.0f;
    float ndcY = (2.0f * pixelY) / viewportHeight - 1.0f; // Matches Vulkan Y-flipped projection

    float aspect = viewportWidth / viewportHeight;
    glm::mat4 invVP = glm::inverse(GetViewProjectionMatrix(aspect));

    glm::vec4 nearPt = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPt  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

    if (std::abs(nearPt.w) > 1e-6f) nearPt /= nearPt.w;
    if (std::abs(farPt.w) > 1e-6f) farPt /= farPt.w;

    ray.origin = glm::vec3(nearPt);
    glm::vec3 dir = glm::vec3(farPt - nearPt);
    float len = glm::length(dir);
    ray.direction = (len > 1e-6f) ? (dir / len) : GetForwardDirection();
    return ray;
}

PickResult ScenePickerAndGizmo::PickObjectByRay(
    const std::vector<core::SceneObject>& objects,
    const WorldRay& ray) {
    PickResult best{};
    best.distance = 1e30f;

    auto findById = [&](core::ObjectId id) -> const core::SceneObject* {
        for (const auto& o : objects) {
            if (o.id == id) return &o;
        }
        return nullptr;
    };

    auto computeWorldMat = [&](const core::SceneObject& obj) -> glm::mat4 {
        glm::mat4 world = obj.transform.ToLocalMatrix();
        core::ObjectId pid = obj.parentId;
        int guard = 0;
        while (pid != 0 && guard < 32) {
            const core::SceneObject* p = findById(pid);
            if (!p || p->id == obj.id) break;
            world = p->transform.ToLocalMatrix() * world;
            pid = p->parentId;
            ++guard;
        }
        return world;
    };

    for (const auto& obj : objects) {
        if (!obj.visible || !obj.mesh.visible) continue;

        glm::mat4 worldMat = computeWorldMat(obj);
        glm::mat4 invWorld = glm::inverse(worldMat);

        glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localDirUnnorm = glm::vec3(invWorld * glm::vec4(ray.direction, 0.0f));
        float dirLen = glm::length(localDirUnnorm);
        if (dirLen < 1e-6f) continue;
        glm::vec3 localDir = localDirUnnorm / dirLen;

        creative::Aabb3D box;
        if (obj.mesh.primitiveType == "plane") {
            box.minBounds = glm::vec3(-5.0f, -0.05f, -5.0f);
            box.maxBounds = glm::vec3(5.0f, 0.05f, 5.0f);
        } else if (obj.mesh.primitiveType == "sphere") {
            box.minBounds = glm::vec3(-0.6f);
            box.maxBounds = glm::vec3(0.6f);
        } else if (obj.mesh.primitiveType == "torus") {
            box.minBounds = glm::vec3(-0.9f, -0.3f, -0.9f);
            box.maxBounds = glm::vec3(0.9f, 0.3f, 0.9f);
        } else {
            box.minBounds = glm::vec3(-0.5f);
            box.maxBounds = glm::vec3(0.5f);
        }

        float localDist = 0.0f;
        if (box.IntersectsRay(localOrigin, localDir, &localDist)) {
            glm::vec3 localHit = localOrigin + localDir * localDist;
            glm::vec3 worldHit = glm::vec3(worldMat * glm::vec4(localHit, 1.0f));
            float worldDist = glm::length(worldHit - ray.origin);

            if (worldDist < best.distance) {
                best.hit = true;
                best.objectId = obj.id;
                best.distance = worldDist;
                best.worldHitPoint = worldHit;
            }
        }
    }

    return best;
}

float ScenePickerAndGizmo::SnapValue(float value, float step) {
    if (step <= 1e-5f) return value;
    return std::round(value / step) * step;
}

glm::vec3 ScenePickerAndGizmo::SnapVec3(const glm::vec3& v, float step) {
    return glm::vec3(SnapValue(v.x, step), SnapValue(v.y, step), SnapValue(v.z, step));
}

core::TransformComponent ScenePickerAndGizmo::ApplyGizmoDelta(
    const core::TransformComponent& currentTransform,
    GizmoMode mode,
    const glm::vec3& rawDelta,
    const SnapSettings& snap) {
    core::TransformComponent out = currentTransform;
    if (mode == GizmoMode::Translate) {
        out.position += rawDelta;
        if (snap.enableSnap) {
            out.position = SnapVec3(out.position, snap.translateSnapStep);
        }
    } else if (mode == GizmoMode::Rotate) {
        out.rotationEulerDeg += rawDelta;
        if (snap.enableSnap) {
            out.rotationEulerDeg = SnapVec3(out.rotationEulerDeg, snap.rotateSnapDeg);
        }
    } else if (mode == GizmoMode::Scale) {
        out.scale += rawDelta;
        if (snap.enableSnap) {
            out.scale = SnapVec3(out.scale, snap.scaleSnapStep);
        }
    }
    out.Sanitize();
    return out;
}

} // namespace platz::engine::scene
