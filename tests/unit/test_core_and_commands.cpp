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

TEST_CASE("Gate 4: Non-Destructive 3D Modifier Stack, Visual Material Node Graph -> Slang, 2D Pixel Canvas & 3-Role AI") {
    // 1. Non-Destructive 3D Modifier Stack (PLZ-MODEL-005, PLZ-MODEL-012)
    creative::MeshData baseCube = creative::MeshGenerator::GenerateCube(1.0f);
    std::vector<core::MeshModifier> stack = {
        {"m1", "Subdivide", true, 1.0f, 1.0f, 42},
        {"m2", "TaperTwist", true, 60.0f, 0.55f, 42},
        {"m3", "BevelChamfer", true, 0.12f, 1.0f, 42}
    };
    creative::MeshData evaluated = creative::MeshGenerator::EvaluateModifierStack(baseCube, stack);
    auto quality = evaluated.AnalyzeMeshQuality();
    CHECK(quality.valid);
    CHECK(quality.triangleCount == (baseCube.indices.size() / 3) * 4);

    // 2. Expandable Validated DAG Visual Material Node Graph -> Slang Compilation (PLZ-MAT-001, PLZ-RENDERX-002)
    creative::MaterialGraphDocument graph;
    CHECK(graph.ValidateGraph().valid);

    // Expand beyond default 4 nodes with WaveBands + BlendMix (2-input DAG!)
    std::string waveId = graph.AddNode("WaveBands", {0.10f, 0.92f, 0.98f}, 7.0f, 0.30f, "node_albedo");
    std::string blendId = graph.AddNode("BlendMix", {1.0f, 1.0f, 1.0f}, 0.50f, 0.0f, waveId, "node_checker");
    REQUIRE(graph.ConnectNodes(blendId, "node_fresnel", 0));
    auto expVal = graph.ValidateGraph();
    CHECK(expVal.valid);
    CHECK(graph.GetNodes().size() == 6);
    CHECK(expVal.topologicalOrder.size() == 6);

    // Attempt to introduce a cycle (node_fresnel -> waveId) via ConnectNodes: must be rejected and rolled back cleanly!
    std::string cycleErr;
    CHECK_FALSE(graph.ConnectNodes("node_fresnel", waveId, 0, &cycleErr));
    CHECK((cycleErr.find("ycle") != std::string::npos || cycleErr.find("DAG") != std::string::npos));
    CHECK(graph.ValidateGraph().valid);

    std::string slangCode, failNode, err;
    CHECK(graph.CompileToSlangShaderSource(&slangCode, &failNode, &err));
    CHECK(slangCode.find("DistributionGGX") != std::string::npos);
    CHECK(slangCode.find("sin(") != std::string::npos);
    CHECK(slangCode.find("lerp(") != std::string::npos);

    // Remove a node and verify downstream rewiring preserves DAG validity
    CHECK(graph.RemoveNode(waveId));
    CHECK(graph.ValidateGraph().valid);

    // Introduce a broken node link directly and verify PLZ-RENDERX-002 reports the exact failing node ID
    graph.FindNode("node_fresnel")->inputNodeA = "nonexistent_node_99";
    auto badVal = graph.ValidateGraph();
    CHECK_FALSE(badVal.valid);
    CHECK(badVal.failingNodeId == "node_fresnel");

    // 3. Multi-Layer 2D Pixel-Art Canvas with Mirror-X Symmetry, Layer Compositing & Binary PLZT Serialization
    creative::PixelArtCanvas canvas(16, 16);
    CHECK(canvas.layers.size() == 2);
    uint32_t layer3 = canvas.AddLayer("AccentSparks");
    CHECK(layer3 == 2);
    canvas.SetIndex(2, 4, 5, /*mirrorX=*/true);
    CHECK(canvas.GetIndex(2, 4) == 5);
    CHECK(canvas.GetIndex(13, 4) == 5); // Mirrored across 16px width!
    auto bakedSprite = canvas.BakeToImageData2D();
    CHECK(bakedSprite.pixelExactNearest);
    CHECK(bakedSprite.width == 16);
    CHECK(bakedSprite.GetPixel(2, 4) == canvas.palette[5]);

    auto plztBlob = bakedSprite.SerializeToBinaryBlob();
    creative::ImageData2D decodedSprite;
    REQUIRE(creative::ImageData2D::DeserializeFromBinaryBlob(plztBlob, &decodedSprite));
    CHECK(decodedSprite.width == 16);
    CHECK(decodedSprite.pixelExactNearest);
    CHECK(decodedSprite.GetPixel(2, 4) == canvas.palette[5]);

    // 4. Honest Measured Holdout Metrics on Dual Native Specialists + Coordinated 3-Role AI Workflow
    core::ProjectState proj;
    core::ObjectId targetId = proj.CreateObject("HeroDrone", "cube").id;

    ai::NativeMaterialSpecialistModel matSpec;
    std::vector<ai::TrainingSample> tr, ho;
    ai::NativeMaterialSpecialistModel::BuildDefaultCuratedDataset(&tr, &ho);
    ai::TrainingHyperparameters hp;
    auto matRec = matSpec.TrainAndEvaluate(tr, ho, hp);
    CHECK(matRec.convergedAndQualified);
    CHECK(matRec.finalHoldoutMse < matRec.baselineHoldoutMse * 0.35f);
    CHECK(matRec.holdoutRSquared > 0.75f);
    CHECK(matRec.holdoutMae > 0.0f);
    CHECK(matRec.holdoutMae < 0.12f);
    CHECK(matRec.inferenceTimingPasses >= 10000u);
    CHECK(matRec.avgInferenceLatencyUs > 0.0);

    ai::NativeGameplayBalanceSpecialistModel gpSpec;
    auto gpRec = gpSpec.TrainDefaultGameplaySpecialist();
    CHECK(gpRec.convergedAndQualified);
    CHECK(gpRec.finalHoldoutMse < gpRec.baselineHoldoutMse * 0.35f);
    CHECK(gpRec.holdoutRSquared > 0.70f);
    CHECK(gpRec.holdoutMae > 0.0f);
    CHECK(gpRec.holdoutMae < 0.12f);
    CHECK(gpRec.inferenceTimingPasses >= 8000u);
    CHECK(gpRec.avgInferenceLatencyUs > 0.0);

    ai::CommandRegistry reg;
    ai::StagedAiSession session(reg);
    auto roleRec = ai::ThreeRoleCoordinator::PlanSynthesizeAndVerifyEnhancement(
        proj, targetId, 1, 2, matSpec, gpSpec, session);
    CHECK(roleRec.stagedSuccessfully);
    CHECK(roleRec.commandsStaged == 2);
    CHECK(session.ApproveAndCommit(roleRec.sessionToken, proj));
    CHECK_FALSE(proj.FindObject(targetId)->logic.rules.empty());

    // 5. Unified Keyframed Animation Evaluator (Linear, EaseInOut, Step) + Typed Commands
    core::SceneObject animObj;
    animObj.animation.enabled = true;
    animObj.animation.loop = true;
    animObj.animation.durationSec = 2.0f;
    animObj.animation.tracks.push_back({"tr_pos", "position", {{0.0f, {0.0f, 0.0f, 0.0f}, "Linear"}, {2.0f, {10.0f, 0.0f, 0.0f}, "Linear"}}});
    animObj.animation.tracks.push_back({"tr_scl", "scale", {{0.0f, {1.0f, 1.0f, 1.0f}, "Step"}, {2.0f, {3.0f, 3.0f, 3.0f}, "Step"}}});
    animObj.animation.tracks.push_back({"tr_em", "emissiveStrength", {{0.0f, {0.0f, 0.0f, 0.0f}, "EaseInOut"}, {2.0f, {4.0f, 0.0f, 0.0f}, "EaseInOut"}}});

    engine::runtime::RuntimeSession::EvaluateObjectAnimationAtTime(animObj, 0.5);
    CHECK(animObj.transform.position.x == doctest::Approx(2.5f));
    CHECK(animObj.transform.scale.x == doctest::Approx(1.0f)); // Step holds previous keyframe until t=2.0
    // Smoothstep at u=0.25: 3*(0.25)^2 - 2*(0.25)^3 = 0.15625 * 4.0 = 0.625
    CHECK(animObj.material.emissiveStrength == doctest::Approx(0.625f));

    // Verify new CommandRegistry commands for Animation & Audio + invalid Material DAG rejection
    ai::CapabilityGrant grant;
    ai::TypedCommandRequest kfReq;
    kfReq.commandName = "object.add_animation_keyframe";
    kfReq.expectedRevision = proj.GetHeadRevision();
    kfReq.targetObjectId = targetId;
    kfReq.parameters = {{"propertyPath", "position"}, {"timeSec", 1.5f}, {"value", {2.0f, 3.0f, 4.0f}}, {"interpolation", "EaseInOut"}};
    CHECK(reg.ValidateAndApplyCommand(proj, grant, kfReq).valid);
    CHECK(proj.FindObject(targetId)->animation.enabled);
    CHECK(proj.FindObject(targetId)->animation.tracks.size() == 1);

    ai::TypedCommandRequest audReq;
    audReq.commandName = "object.set_audio_source";
    audReq.expectedRevision = proj.GetHeadRevision();
    audReq.targetObjectId = targetId;
    audReq.parameters = {{"enabled", true}, {"cueType", "CrystalChime"}, {"volume", 0.7f}, {"pitch", 1.2f}};
    CHECK(reg.ValidateAndApplyCommand(proj, grant, audReq).valid);
    CHECK(proj.FindObject(targetId)->audio.enabled);
    CHECK(proj.FindObject(targetId)->audio.cueType == "CrystalChime");

    ai::TypedCommandRequest badGraphReq;
    badGraphReq.commandName = "object.set_material_graph";
    badGraphReq.expectedRevision = proj.GetHeadRevision();
    badGraphReq.targetObjectId = targetId;
    badGraphReq.parameters = {{"materialGraphJson", "{broken json!!!"}};
    CHECK_FALSE(reg.ValidateCommand(proj, grant, badGraphReq).valid);
}
