#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/project_state.h"
#include "creative/asset_importer.h"
#include "creative/mesh_generator.h"
#include "engine/runtime/runtime_session.h"
#include "platform/platform_paths.h"
#include "studio/validation_world.h"

#include <filesystem>

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

TEST_CASE("J-06: Living Validation World (projects/PLATZ-Validation) All 6 Stations & Station Reset") {
    std::filesystem::path valDir = platform::GetRepoRootDir() / "projects" / "PLATZ-Validation";
    core::ProjectState valProj;
    std::string err;
    REQUIRE(studio::ValidationWorldManager::EnsureValidationWorldPopulated(valProj, valDir, &err));

    auto receipt = studio::ValidationWorldManager::RunAutomatedVerificationRoute(valProj);
    CHECK(receipt.stationsVisited == 6);
    CHECK(receipt.stationsPassed == 6);
    CHECK(receipt.allStationsPassed);

    // PLZ-TEST-008: Reset a single station and re-verify
    REQUIRE(studio::ValidationWorldManager::ResetStationToBaseline(valProj, "ST-04", &err));
    auto receiptAfterReset = studio::ValidationWorldManager::RunAutomatedVerificationRoute(valProj);
    CHECK(receiptAfterReset.allStationsPassed);
}
