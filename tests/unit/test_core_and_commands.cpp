#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "ai/command_registry.h"
#include "ai/specialist_model.h"
#include "ai/staged_session.h"
#include "core/object_identity.h"
#include "core/project_state.h"
#include "creative/asset_importer.h"
#include "creative/mesh_generator.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/runtime/runtime_session.h"
#include "engine/scene/render_scene.h"
#include "platform/platform_paths.h"
#include "studio/studio_ui.h"

#include <cmath>
#include <limits>

using namespace platz;

TEST_CASE("FIPS 180-4 SHA-256 and Stable ObjectIdentity") {
    // Known test vector: SHA-256("abc")
    CHECK(platform::ComputeSha256Hex(std::string_view("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    core::ObjectId id1 = core::GenerateObjectId();
    core::ObjectId id2 = core::GenerateObjectId();
    CHECK(id1 != 0);
    CHECK(id2 != 0);
    CHECK(id1 != id2);
    CHECK(core::FormatObjectId(id1).starts_with("OBJ-"));
}

TEST_CASE("N-10 Transform NaN/Inf Sanitization and Hierarchy World Matrix (PLZ-GAME-001)") {
    core::TransformComponent badTransform;
    badTransform.position = {std::numeric_limits<float>::quiet_NaN(), 5.0f, std::numeric_limits<float>::infinity()};
    badTransform.scale = {0.0f, -0.000001f, 2.0f};
    CHECK_FALSE(badTransform.IsFinite());

    badTransform.Sanitize();
    CHECK(badTransform.IsFinite());
    CHECK(badTransform.position.x == doctest::Approx(0.0f));
    CHECK(badTransform.position.y == doctest::Approx(5.0f));
    CHECK(badTransform.scale.x == doctest::Approx(1.0f));

    core::ProjectState proj;
    core::ObjectId parentId = proj.CreateObject("ParentNode", "cube").id;
    proj.FindObject(parentId)->transform.position = {10.0f, 2.0f, -4.0f};

    core::ObjectId childId = proj.CreateObject("ChildNode", "sphere", parentId).id;
    proj.FindObject(childId)->transform.position = {0.0f, 3.0f, 1.0f};

    glm::mat4 childWorld = proj.ComputeWorldMatrix(childId);
    glm::vec3 worldPos = glm::vec3(childWorld[3]);
    CHECK(worldPos.x == doctest::Approx(10.0f));
    CHECK(worldPos.y == doctest::Approx(5.0f));
    CHECK(worldPos.z == doctest::Approx(-3.0f));
}

TEST_CASE("N-01 Identity Collision Disambiguation and N-06 Mixed-Modality Undo/Redo") {
    core::ProjectState proj;
    core::ObjectId twinAId = proj.CreateObject("SharedNameCrystal", "cube").id;
    core::ObjectId twinBId = proj.CreateObject("SharedNameCrystal", "sphere").id;
    CHECK(twinAId != twinBId);

    auto matches = proj.FindObjectsByName("SharedNameCrystal");
    CHECK(matches.size() == 2);

    // Edit twinA transform, then twinB material, then selectively undo/redo (N-06)
    core::SceneObject modA = *proj.FindObject(twinAId);
    modA.transform.position = {4.0f, 5.0f, 6.0f};
    proj.ApplyObjectMutation(twinAId, modA, "Move Twin A", "Scene");

    core::SceneObject modB = *proj.FindObject(twinBId);
    modB.material.metallic = 0.91f;
    modB.material.roughness = 0.18f;
    proj.ApplyObjectMutation(twinBId, modB, "Edit Twin B Material", "Material");

    CHECK(proj.FindObject(twinAId)->transform.position.x == doctest::Approx(4.0f));
    CHECK(proj.FindObject(twinBId)->material.metallic == doctest::Approx(0.91f));

    // Undo material edit on Twin B; Twin A transform edit must remain intact
    std::string undoneLabel;
    CHECK(proj.Undo(&undoneLabel));
    CHECK(undoneLabel == "Edit Twin B Material");
    CHECK(proj.FindObject(twinBId)->material.metallic != doctest::Approx(0.91f));
    CHECK(proj.FindObject(twinAId)->transform.position.x == doctest::Approx(4.0f));

    // Redo material edit on Twin B
    CHECK(proj.Redo(&undoneLabel));
    CHECK(proj.FindObject(twinBId)->material.metallic == doctest::Approx(0.91f));
}

TEST_CASE("Gate 2 Transform Gizmo Snapping, 3D Ray Picking, OBJ Round-Trip (J-03), and N-14 Pixel Exactness") {
    engine::scene::SnapSettings snap{};
    snap.enableSnap = true;
    snap.translateSnapStep = 0.5f;
    snap.rotateSnapDeg = 15.0f;
    snap.scaleSnapStep = 0.25f;

    core::TransformComponent t{};
    auto moved = engine::scene::ScenePickerAndGizmo::ApplyGizmoDelta(
        t, engine::scene::GizmoMode::Translate, glm::vec3(0.49f, 1.02f, -0.74f), snap);
    CHECK(moved.position.x == doctest::Approx(0.5f));
    CHECK(moved.position.y == doctest::Approx(1.0f));
    CHECK(moved.position.z == doctest::Approx(-0.5f));

    // 3D Ray Picking
    core::ProjectState proj;
    auto& target = proj.CreateObject("PickTarget", "cube");
    target.transform.position = {0.0f, 1.0f, 0.0f};

    engine::scene::ViewportCamera cam;
    cam.FocusOnPoint({0.0f, 1.0f, 0.0f}, 6.0f);
    auto centerRay = cam.ScreenPointToWorldRay(640.0f, 360.0f, 1280.0f, 720.0f);
    auto pick = engine::scene::ScenePickerAndGizmo::PickObjectByRay(proj.GetObjects(), centerRay);
    CHECK(pick.hit);
    CHECK(pick.objectId == target.id);

    // J-03 Mesh Subdivision + Wavefront OBJ Export/Import Round-Trip
    creative::MeshData cube = creative::MeshGenerator::GenerateCube(1.0f);
    CHECK(cube.ValidateTopology());
    creative::MeshData subdiv = creative::MeshGenerator::ApplySubdivisionModifier(cube);
    CHECK(subdiv.ValidateTopology());
    CHECK(subdiv.indices.size() == cube.indices.size() * 4);

    std::string objStr = creative::AssetImporter::ExportMeshToObjString(subdiv);
    creative::MeshData reimported;
    std::string err;
    REQUIRE(creative::AssetImporter::LoadObjFromString(objStr, "RoundTripSubdiv", &reimported, &err));
    CHECK(reimported.indices.size() == subdiv.indices.size());

    // N-14 Indexed Pixel-Art Sprite Exactness under 4x Zoom
    std::vector<creative::RgbaColor8> palette = {
        {12, 18, 28, 255},
        {0, 218, 245, 255},
        {248, 196, 42, 255}
    };
    std::vector<uint8_t> indices = {
        0, 1, 1, 0,
        1, 2, 2, 1,
        1, 2, 2, 1,
        0, 1, 1, 0
    };
    auto sprite = creative::TextureGenerator::GenerateIndexedPixelArtSprite(4, 4, palette, indices);
    CHECK(sprite.pixelExactNearest);
    auto zoomed = creative::TextureGenerator::SampleNearestNeighborExact(sprite, 4);
    CHECK(zoomed.width == 16);
    CHECK(zoomed.height == 16);
    // Verify exact indexed pixel color with zero bilinear bleeding at boundaries
    CHECK(zoomed.GetPixel(3, 3) == palette[0]);
    CHECK(zoomed.GetPixel(4, 4) == palette[2]);
}

TEST_CASE("J-04 Universal Command Bar, Honest Backlog Status (PLZ-UXPLUS-008), and Accessibility Announcer (N-29)") {
    core::ProjectState proj;
    engine::vulkan::VulkanRenderer dummyRenderer;
    engine::runtime::RuntimeSession runtime;
    ai::CommandRegistry reg;
    ai::StagedAiSession staged(reg);
    ai::NativeMaterialSpecialistModel specialist;

    studio::StudioUiController ui(proj, dummyRenderer, runtime, reg, staged, specialist);

    auto searchResults = ui.SearchCommandBar("sphere");
    REQUIRE_FALSE(searchResults.empty());
    CHECK(searchResults.front().commandId == "create.sphere");

    std::string statusMsg;
    CHECK(ui.InvokeCommandById("create.sphere", &statusMsg));
    CHECK(proj.GetObjects().size() == 1);
    CHECK(ui.GetAccessibility().GetLastAnnouncement().find("Created") != std::string::npos);

    // Honest completion state: invoking a long-term backlog command must return false and disclose backlog status
    CHECK_FALSE(ui.InvokeCommandById("future.raytracing_pathtracer", &statusMsg));
    CHECK(statusMsg.find("BACKLOG") != std::string::npos);
}
