#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "ai/command_registry.h"
#include "ai/specialist_model.h"
#include "ai/staged_session.h"
#include "core/project_state.h"
#include "creative/asset_importer.h"
#include "creative/mesh_generator.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/shader_compiler.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/runtime/runtime_session.h"
#include "platform/audio_engine.h"
#include "platform/platform_paths.h"
#include "studio/studio_ui.h"
#include "studio/validation_world.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace platz;

namespace {

std::filesystem::path MakeCleanTestProjectDir(const std::string& name) {
    std::filesystem::path dir = platform::GetRepoRootDir() / "out" / "test_projects" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("J-01 & N-03: SQLite + Content-Addressed Asset Store Atomic Persistence & Interrupted Save Recovery") {
    std::filesystem::path projDir = MakeCleanTestProjectDir("proj_atomic_persistence");

    core::ObjectId savedObjId = 0;
    core::AssetId savedAssetId = 0;
    core::RevisionId rev1 = 0;

    {
        core::ProjectState proj;
        std::string err;
        REQUIRE(proj.CreateOrOpenProject(projDir, "AtomicTestProject", &err));

        creative::MeshData torus = creative::MeshGenerator::GenerateTorus(0.7f, 0.2f, 24, 12);
        auto imp = creative::AssetImporter::ImportMeshIntoProject(
            proj, torus, "HeroTorusAsset", "assets/torus.obj", "CC0-1.0", &savedObjId);
        REQUIRE(imp.success);
        savedAssetId = imp.assetId;

        auto* obj = proj.FindObject(savedObjId);
        REQUIRE(obj != nullptr);
        obj->transform.position = {3.5f, 2.25f, -1.75f};
        obj->material.metallic = 0.88f;
        obj->material.roughness = 0.19f;

        REQUIRE(proj.SaveProject("Initial commit with custom asset", "Tester", &err));
        rev1 = proj.GetHeadRevision();
        CHECK(rev1 == 1);

        // N-03: Simulate an interrupted save / crash between staging a blob and advancing the SQLite revision head
        auto ticket = proj.GetAssetStore().StageBlob(torus.SerializeToBinaryBlob());
        CHECK(ticket.valid);
        obj->transform.position = {999.0f, 999.0f, 999.0f};
        CHECK_FALSE(proj.SaveProject("Interrupted commit", "Tester", &err, /*injectFaultBeforeCommit=*/true));
    }

    // Reopen in a completely new ProjectState instance: must recover Revision 1 and clean up orphaned staging files
    {
        core::ProjectState reopened;
        std::string err;
        REQUIRE(reopened.CreateOrOpenProject(projDir, "AtomicTestProject", &err));
        CHECK(reopened.GetHeadRevision() == rev1);

        const auto* obj = reopened.FindObject(savedObjId);
        REQUIRE(obj != nullptr);
        CHECK(obj->transform.position.x == doctest::Approx(3.5f));
        CHECK(obj->transform.position.y == doctest::Approx(2.25f));
        CHECK(obj->material.metallic == doctest::Approx(0.88f));

        const auto* ast = reopened.FindAsset(savedAssetId);
        REQUIRE(ast != nullptr);
        CHECK(reopened.GetAssetStore().HasVerifiedBlob(ast->contentSha256));
    }
}

TEST_CASE("N-05: Source-to-Derived Dependency Invalidation (PLZ-CORE-005, PLZ-ARCH-004)") {
    std::filesystem::path projDir = MakeCleanTestProjectDir("proj_dependency_invalidation");
    core::ProjectState proj;
    REQUIRE(proj.CreateOrOpenProject(projDir, "DependencyTest"));

    creative::MeshData meshA = creative::MeshGenerator::GenerateCube(1.0f);
    creative::MeshData meshB = creative::MeshGenerator::GenerateSphere(0.5f, 12, 8);

    core::ObjectId objA1 = 0, objB1 = 0;
    auto impA = creative::AssetImporter::ImportMeshIntoProject(proj, meshA, "SharedMeshA", "", "CC0-1.0", &objA1);
    auto impB = creative::AssetImporter::ImportMeshIntoProject(proj, meshB, "IndependentMeshB", "", "CC0-1.0", &objB1);
    REQUIRE(impA.success);
    REQUIRE(impB.success);

    // Create a second instance referencing SharedMeshA
    auto& objA2 = proj.CreateObject("SecondInstanceOfA", "custom_asset");
    objA2.mesh.meshAssetId = impA.assetId;

    uint64_t genB_before = proj.FindAsset(impB.assetId)->generationVersion;

    // Modify SharedMeshA -> only objA1 and objA2 should be invalidated; IndependentMeshB must remain untouched
    creative::MeshData meshA_subdiv = creative::MeshGenerator::ApplySubdivisionModifier(meshA);
    std::vector<core::ObjectId> invalidated;
    std::string err;
    REQUIRE(proj.UpdateAssetPayload(impA.assetId, meshA_subdiv.SerializeToBinaryBlob(), &invalidated, &err));

    CHECK(invalidated.size() == 2);
    CHECK(proj.FindAsset(impA.assetId)->generationVersion == 2);
    CHECK(proj.FindAsset(impB.assetId)->generationVersion == genB_before);
}

TEST_CASE("J-02, N-07, N-08, N-09: Isolated Play/Stop, Gameplay Checkpoints, and Conserved Inventory Transfers") {
    std::filesystem::path projDir = MakeCleanTestProjectDir("proj_runtime_and_conservation");
    core::ProjectState proj;
    REQUIRE(proj.CreateOrOpenProject(projDir, "RuntimeTest"));

    core::ObjectId genId = proj.CreateObject("Generator", "cube").id;
    proj.FindObject(genId)->transform.position = {0.0f, 1.0f, 0.0f};
    proj.FindObject(genId)->logic.currentState = "Idle";
    proj.FindObject(genId)->logic.inventory["energy_crystal"] = 100;

    core::ObjectId vaultId = proj.CreateObject("Vault", "cylinder").id;
    proj.FindObject(vaultId)->transform.position = {3.0f, 1.0f, 0.0f};
    proj.FindObject(vaultId)->logic.inventory["energy_crystal"] = 50;

    core::BehaviorRule spinRule;
    spinRule.ruleId = "spin";
    spinRule.triggerType = "OnTick";
    spinRule.conditionExpr = "always";
    spinRule.actionType = "RotateY";
    spinRule.paramA = 90.0f;
    proj.FindObject(genId)->logic.rules.push_back(spinRule);

    REQUIRE(proj.SaveProject("Save authoring baseline before Play"));
    core::RevisionId authoringRevBeforePlay = proj.GetHeadRevision();

    engine::runtime::RuntimeSession rt;
    REQUIRE(rt.StartPlay(proj));

    // Step simulation (clamped to 0.1s max frame step * 90 deg/s = 9.0 deg) and verify runtime object rotated
    rt.StepSimulation(0.5);
    CHECK(rt.FindRuntimeObject(genId)->transform.rotationEulerDeg.y == doctest::Approx(9.0f));
    CHECK_FALSE(rt.GetDebuggerTimeline().empty());

    // N-09: Conserved resource transfer (normal + simulated mid-transfer interruption)
    CHECK(rt.ComputeTotalResourceAcrossWorld("energy_crystal") == 150);
    std::string err;
    CHECK(rt.ExecuteConservedResourceTransfer(genId, vaultId, "energy_crystal", 30, /*simulateInterruption=*/false, &err));
    CHECK(rt.FindRuntimeObject(genId)->logic.inventory["energy_crystal"] == 70);
    CHECK(rt.FindRuntimeObject(vaultId)->logic.inventory["energy_crystal"] == 80);
    CHECK(rt.ComputeTotalResourceAcrossWorld("energy_crystal") == 150);

    // Now inject an interruption mid-transfer: must rollback with zero lost or duplicated items!
    CHECK_FALSE(rt.ExecuteConservedResourceTransfer(genId, vaultId, "energy_crystal", 40, /*simulateInterruption=*/true, &err));
    CHECK(rt.FindRuntimeObject(genId)->logic.inventory["energy_crystal"] == 70);
    CHECK(rt.FindRuntimeObject(vaultId)->logic.inventory["energy_crystal"] == 80);
    CHECK(rt.ComputeTotalResourceAcrossWorld("energy_crystal") == 150);

    // N-08: Save gameplay checkpoint separately from editor authoring revision
    REQUIRE(rt.SaveGameplayCheckpoint(proj, "slot_alpha", &err));
    CHECK(proj.GetHeadRevision() == authoringRevBeforePlay);

    // Mutate runtime further, then reload checkpoint
    CHECK(rt.ExecuteConservedResourceTransfer(genId, vaultId, "energy_crystal", 20, false, &err));
    CHECK(rt.FindRuntimeObject(genId)->logic.inventory["energy_crystal"] == 50);

    REQUIRE(rt.LoadGameplayCheckpoint(proj, "slot_alpha", &err));
    CHECK(rt.FindRuntimeObject(genId)->logic.inventory["energy_crystal"] == 70);
    CHECK(rt.FindRuntimeObject(vaultId)->logic.inventory["energy_crystal"] == 80);

    // N-07: Stop Play -> canonical authoring state must be completely untouched!
    rt.StopPlay();
    CHECK(proj.GetHeadRevision() == authoringRevBeforePlay);
    CHECK(proj.FindObject(genId)->transform.rotationEulerDeg.y == doctest::Approx(0.0f));
    CHECK(proj.FindObject(genId)->logic.inventory["energy_crystal"] == 100);
    CHECK(proj.FindObject(vaultId)->logic.inventory["energy_crystal"] == 50);
}

TEST_CASE("J-06: Living Validation World (projects/PLATZ-Validation) All 9 Stations & Station Reset") {
    std::filesystem::path valDir = platform::GetRepoRootDir() / "projects" / "PLATZ-Validation";
    core::ProjectState valProj;
    std::string err;
    REQUIRE(studio::ValidationWorldManager::EnsureValidationWorldPopulated(valProj, valDir, &err));

    auto receipt = studio::ValidationWorldManager::RunAutomatedVerificationRoute(valProj);
    CHECK(receipt.stationsVisited == 9);
    CHECK(receipt.stationsPassed == 9);
    CHECK(receipt.allStationsPassed);

    // PLZ-TEST-008: Reset a single station and re-verify
    REQUIRE(studio::ValidationWorldManager::ResetStationToBaseline(valProj, "ST-04", &err));
    auto receiptAfterReset = studio::ValidationWorldManager::RunAutomatedVerificationRoute(valProj);
    CHECK(receiptAfterReset.allStationsPassed);
}

TEST_CASE("Complete Save/Reopen/Edit/Undo/Redo Round-Trip for Expandable Material DAG and Multi-Layer Pixel-Art Canvas") {
    std::filesystem::path projDir = MakeCleanTestProjectDir("proj_creative_roundtrip");

    core::ObjectId matObjId = 0;
    core::ObjectId pixObjId = 0;
    std::string addedWaveNodeId;

    // Phase 1: Create two objects, author a custom 6-node Material DAG on matObj and a 3-layer Pixel Canvas on pixObj, Undo/Redo, Save!
    {
        core::ProjectState proj;
        std::string err;
        REQUIRE(proj.CreateOrOpenProject(projDir, "CreativeRoundTripTest", &err));

        matObjId = proj.CreateObject("CustomDagPillar", "cube").id;
        pixObjId = proj.CreateObject("EditableSpriteCrate", "cube").id;

        engine::vulkan::VulkanRenderer dummyRenderer;
        engine::runtime::RuntimeSession rt;
        ai::CommandRegistry reg;
        ai::StagedAiSession staged(reg);
        ai::NativeMaterialSpecialistModel matSpec;
        studio::StudioUiController ui(proj, dummyRenderer, rt, reg, staged, matSpec);

        // Author custom 6-node Material DAG on matObj
        ui.SetSelectedObjectId(matObjId);
        addedWaveNodeId = ui.GetMaterialGraph().AddNode("WaveBands", {0.2f, 0.9f, 0.8f}, 9.5f, 0.42f, "node_albedo");
        std::string gradId = ui.GetMaterialGraph().AddNode("HeightGradient", {0.95f, 0.72f, 0.25f}, 0.0f, 2.5f, addedWaveNodeId);
        REQUIRE(ui.GetMaterialGraph().ConnectNodes(gradId, "node_checker", 0));
        REQUIRE(ui.CommitMaterialGraphToSelectedObject("Add WaveBands & HeightGradient to DAG"));
        CHECK(ui.GetMaterialGraph().GetNodes().size() == 6);

        // Author editable 3-layer Pixel-Art Canvas on pixObj, test Undo/Redo!
        ui.SetSelectedObjectId(pixObjId);
        auto& canvas = ui.GetPixelArtCanvas();
        canvas.spriteName = "HeroShield_3Layer";
        uint32_t layer3 = canvas.AddLayer("GoldRunes");
        CHECK(layer3 == 2);
        canvas.SetIndex(3, 3, 2, /*mirrorX=*/false); // Solar Gold on Layer 2
        REQUIRE(ui.CommitPixelCanvasToSelectedObject("Add Layer 3 & Paint Rune A"));

        // Second stroke: paint pixel (5, 5) with Crimson Alert (index 5)
        canvas.SetIndex(5, 5, 5, /*mirrorX=*/false);
        REQUIRE(ui.CommitPixelCanvasToSelectedObject("Paint Rune B"));
        CHECK(ui.GetPixelArtCanvas().GetLayerIndex(2, 5, 5) == 5);

        // Undo second stroke -> (5, 5) must revert to transparent (255), while (3, 3) and Layer 3 remain!
        REQUIRE(ui.InvokeCommandById("edit.undo"));
        CHECK(ui.GetPixelArtCanvas().layers.size() == 3);
        CHECK(ui.GetPixelArtCanvas().GetLayerIndex(2, 3, 3) == 2);
        CHECK(ui.GetPixelArtCanvas().GetLayerIndex(2, 5, 5) == creative::kTransparentPixelIndex);

        // Redo second stroke -> (5, 5) returns to index 5!
        REQUIRE(ui.InvokeCommandById("edit.redo"));
        CHECK(ui.GetPixelArtCanvas().GetLayerIndex(2, 5, 5) == 5);

        // Save project cleanly to SQLite + content-addressed blob store
        REQUIRE(proj.SaveProject("Save custom Material DAG & 3-layer Pixel Canvas", "Tester", &err));
    }

    // Phase 2: Reopen in a completely fresh ProjectState + StudioUiController (simulating close & reopen!)
    {
        core::ProjectState reopened;
        std::string err;
        REQUIRE(reopened.CreateOrOpenProject(projDir, "CreativeRoundTripTest", &err));

        engine::vulkan::VulkanRenderer dummyRenderer;
        engine::runtime::RuntimeSession rt;
        ai::CommandRegistry reg;
        ai::StagedAiSession staged(reg);
        ai::NativeMaterialSpecialistModel matSpec;
        studio::StudioUiController ui(reopened, dummyRenderer, rt, reg, staged, matSpec);

        // Select matObj -> must load the EXACT 6-node custom DAG with WaveBands param 9.5!
        ui.SetSelectedObjectId(matObjId);
        CHECK(ui.GetMaterialGraph().GetNodes().size() == 6);
        const auto* reloadedWave = ui.GetMaterialGraph().FindNode(addedWaveNodeId);
        REQUIRE(reloadedWave != nullptr);
        CHECK(reloadedWave->nodeType == "WaveBands");
        CHECK(reloadedWave->scalarA == doctest::Approx(9.5f));
        CHECK(ui.GetMaterialGraph().ValidateGraph().valid);

        // Select pixObj -> must reopen all 3 unflattened editable layers + exact pixel indices + live texture asset!
        ui.SetSelectedObjectId(pixObjId);
        const auto& reopenedCanvas = ui.GetPixelArtCanvas();
        CHECK(reopenedCanvas.spriteName == "HeroShield_3Layer");
        REQUIRE(reopenedCanvas.layers.size() == 3);
        CHECK(reopenedCanvas.layers[2].layerName == "GoldRunes");
        CHECK(reopenedCanvas.GetLayerIndex(2, 3, 3) == 2);
        CHECK(reopenedCanvas.GetLayerIndex(2, 5, 5) == 5);

        // Modify again after reopening, undo, and verify source stays editable!
        ui.GetPixelArtCanvas().layers[2].visible = false;
        REQUIRE(ui.CommitPixelCanvasToSelectedObject("Hide GoldRunes Layer"));
        CHECK_FALSE(ui.GetPixelArtCanvas().layers[2].visible);
        REQUIRE(ui.InvokeCommandById("edit.undo"));
        CHECK(ui.GetPixelArtCanvas().layers[2].visible);
    }
}

TEST_CASE("N-15: Spatial 3D Audio DSP Panning & Real-Time Audio Stability Under Shader Compile Overload") {
    platform::SpatialAudioEngine audio;
    REQUIRE(audio.Initialize(/*headlessSoftwareOnly=*/true));

    // 1. Verify true 3D stereo panning: source to the left must produce higher left-channel RMS than right-channel RMS
    platform::SpatialAudioCueRequest leftCue;
    leftCue.cueType = "CrystalChime";
    leftCue.sourceWorldPos = {-6.0f, 1.0f, 0.0f};
    leftCue.volume = 0.8f;
    leftCue.spatial3D = true;
    std::vector<float> leftBuf;
    REQUIRE(audio.PlaySpatialCue(leftCue, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, &leftBuf));
    REQUIRE(leftBuf.size() >= 1024);

    double sumL = 0.0, sumR = 0.0;
    for (size_t i = 0; i < 512; ++i) {
        CHECK(std::isfinite(leftBuf[i * 2]));
        CHECK(std::isfinite(leftBuf[i * 2 + 1]));
        sumL += leftBuf[i * 2] * leftBuf[i * 2];
        sumR += leftBuf[i * 2 + 1] * leftBuf[i * 2 + 1];
    }
    CHECK(sumL > sumR * 3.0);

    // 2. N-15 Overload Resilience: Mix continuous real-time audio blocks concurrently while compiling Slang shaders
    std::atomic<bool> audioOk{true};
    std::atomic<bool> stopMixing{false};
    std::atomic<uint64_t> audioBlocksMixed{0};

    auto mixWorker = [&]() {
        std::vector<float> mixChunk;
        int blockIdx = 0;
        while (!stopMixing.load()) {
            if (blockIdx % 16 == 0) {
                platform::SpatialAudioCueRequest c;
                c.cueType = "TransferArp";
                c.sourceWorldPos = {3.0f, 1.0f, 0.0f};
                audio.PlaySpatialCue(c, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
            }
            audio.MixRealtimeBlock(256, &mixChunk);
            for (float s : mixChunk) {
                if (!std::isfinite(s) || std::abs(s) > 1.05f) {
                    audioOk.store(false);
                }
            }
            ++blockIdx;
            audioBlocksMixed.fetch_add(1);
        }
    };
    std::thread audioThread(mixWorker);

    // Hammer Slang 2026.22.1 compilation on main thread concurrently with DSP audio mixing
    engine::vulkan::ShaderCompiler compiler;
    creative::MaterialGraphDocument dag;
    std::string slangSrc;
    REQUIRE(dag.CompileToSlangShaderSource(&slangSrc));
    std::string compileDiag;
    bool compiled = compiler.HotReloadSlangSource("n15_stress", slangSrc, "vsMain", "fsMain", &compileDiag);

    stopMixing.store(true);
    audioThread.join();

    CHECK(compiled);
    CHECK(audioOk.load());
    CHECK(audioBlocksMixed.load() > 10);
    CHECK(audio.GetMetrics().measuredUnderrunCount == 0);
    audio.Shutdown();
}
