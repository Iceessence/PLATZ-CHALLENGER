#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/project_state.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/spirv_validator.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/scene/render_scene.h"
#include "platform/platform_paths.h"
#include "platform/sdl_window.h"
#include "studio/validation_world.h"

using namespace platz;

TEST_CASE("Vulkan 1.4 Renderer, Slang SPIR-V Validation, GPU Readback, Picking, N-04, N-11, N-12, N-13") {
    std::filesystem::path valDir = platform::GetRepoRootDir() / "projects" / "PLATZ-Validation";
    core::ProjectState project;
    std::string err;
    REQUIRE(studio::ValidationWorldManager::EnsureValidationWorldPopulated(project, valDir, &err));

    platform::WindowConfig winCfg{};
    winCfg.headless = true;
    winCfg.width = 640;
    winCfg.height = 360;

    platform::SdlWindow window;
    REQUIRE(window.Initialize(winCfg, &err));

    engine::vulkan::VulkanRenderer renderer;
    REQUIRE(renderer.Initialize(&window, 640, 360, /*enableValidation=*/true, &err));

    const auto& caps = renderer.GetContext().GetCapabilities();
    MESSAGE("Active Vulkan 1.4 Physical Device: " << caps.deviceName
            << " (API " << renderer.GetContext().GetFormattedApiVersion() << ")");
    CHECK(caps.supportsDynamicRendering);
    CHECK(caps.supportsSynchronization2);

    // 1. Render offscreen 3D scene and verify GPU pixel readback & GPU R32_UINT ObjectId picking
    engine::scene::ViewportCamera camera;
    core::ObjectId targetId = project.GetObjects().front().id;
    for (const auto& o : project.GetObjects()) {
        if (o.name == "ST02_PBR_Brushed_Titanium") {
            targetId = o.id;
            camera.FocusOnPoint(o.transform.position, 2.8f);
            break;
        }
    }

    REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));

    creative::ImageData2D img;
    REQUIRE(renderer.ReadbackViewportColorRgba(&img, &err));
    CHECK(img.width == 640);
    CHECK(img.height == 360);

    // Verify rendered image contains genuine lit 3D pixels (non-uniform and non-zero)
    size_t nonClearPixels = 0;
    creative::RgbaColor8 topCorner = img.GetPixel(4, 4);
    for (uint32_t y = 40; y < 320; y += 4) {
        for (uint32_t x = 40; x < 600; x += 4) {
            if (!(img.GetPixel(x, y) == topCorner)) {
                ++nonClearPixels;
            }
        }
    }
    CHECK(nonClearPixels > 100);

    // Verify GPU R32_UINT picking buffer at viewport center hits the focused object
    core::ObjectId gpuPickedId = 0;
    REQUIRE(renderer.ReadbackPickedObjectIdAtPixel(320, 180, &gpuPickedId, &err));
    CHECK(gpuPickedId == targetId);

    // 2. Verify O(1) steady-state GPU resource sync fast-path when project has no mutations
    uint64_t skipsBefore = renderer.GetPerfMetrics().gpuSyncSkipsCount;
    REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));
    CHECK(renderer.GetPerfMetrics().gpuSyncSkipsCount > skipsBefore);

    // 2b. Verify ST-07 5-node DAG Slang compilation hot-reloads cleanly into the live Vulkan 1.4 pipeline!
    for (const auto& o : project.GetObjects()) {
        if (o.logic.stationCategory == "ST-07" && !o.material.materialGraphJson.empty()) {
            auto dag = creative::MaterialGraphDocument::FromJsonString(o.material.materialGraphJson);
            std::string dagSlang;
            REQUIRE(dag.CompileToSlangShaderSource(&dagSlang));
            std::string dagDiag;
            CHECK(renderer.HotReloadPbrShaderFromSlangSource(dagSlang, &dagDiag));
            break;
        }
    }

    // 2c. Focus ST-08 (Pixel-Exact PLZT custom texture bound on 3D Patrol Drone) and verify live textured render
    for (const auto& o : project.GetObjects()) {
        if (o.logic.stationCategory == "ST-08") {
            CHECK(o.material.albedoTextureAssetId != 0);
            camera.FocusOnPoint(o.transform.position, 2.8f);
            REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, o.id, &err));
            core::ObjectId pickedDrone = 0;
            REQUIRE(renderer.ReadbackPickedObjectIdAtPixel(320, 180, &pickedDrone, &err));
            CHECK(pickedDrone == o.id);
            break;
        }
    }

    // 3. N-04: Corrupt / clear derived GPU mesh & texture caches and verify automatic rebuild from canonical blobs
    uint32_t rebuildsBefore = renderer.GetPerfMetrics().rebuiltCacheCount;
    renderer.InvalidateDerivedGpuCaches();
    REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));
    CHECK(renderer.GetPerfMetrics().rebuiltCacheCount > rebuildsBefore);

    // 3. N-13: Shader compile failure during hot-reload keeps last-known-good pipeline operational
    std::string shaderDiag;
    bool badReload = renderer.HotReloadPbrShaderFromSlangSource(
        "THIS_IS_INVALID_SLANG_SHADER_CODE { syntax_error !!! }",
        &shaderDiag);
    CHECK_FALSE(badReload);
    CHECK_FALSE(shaderDiag.empty());
    CHECK(renderer.GetShaderCompiler().GetHotReloadFallbackCount() >= 1);

    // Rendering still succeeds with last-known-good pipeline after failed hot-reload!
    REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));

    // 4. N-12: VMA GPU Memory Budget & Contention Pressure Detection (PLZ-RENDERX-006)
    auto budgetNormal = renderer.GetContext().GetMemoryBudgetSnapshot();
    CHECK(budgetNormal.activeAllocationCount > 0);
    CHECK(budgetNormal.activeAllocatedBytes > 0);
    renderer.GetContext().SetSoftBudgetLimits(1024, 1024); // Set 1 KB soft limit to trigger contention flag
    auto budgetContended = renderer.GetContext().GetMemoryBudgetSnapshot();
    CHECK(budgetContended.underContentionPressure);
    renderer.GetContext().SetSoftBudgetLimits(512ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL);

    // 5. N-11: Simulated GPU Device Loss & Graceful Recovery without Project Corruption
    renderer.GetContext().SimulateDeviceLossForTesting(true);
    CHECK(renderer.GetContext().IsDeviceLost());
    CHECK_FALSE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));
    CHECK(renderer.GetContext().AttemptRecoverFromSimulatedDeviceLoss());
    CHECK_FALSE(renderer.GetContext().IsDeviceLost());
    REQUIRE(renderer.RenderSceneOffscreen(project, project.GetObjects(), camera, targetId, &err));

    // 6. Verify 0 Vulkan validation errors across the entire test session
    CHECK(renderer.GetContext().GetValidationErrorCount() == 0);

    renderer.Shutdown();
    window.Shutdown();
}
