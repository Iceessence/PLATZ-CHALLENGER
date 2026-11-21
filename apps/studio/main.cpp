#include "ai/command_registry.h"
#include "ai/specialist_model.h"
#include "ai/staged_session.h"
#include "core/project_state.h"
#include "creative/texture_generator.h"
#include "engine/render/vulkan/vulkan_renderer.h"
#include "engine/runtime/runtime_session.h"
#include "platform/platform_paths.h"
#include "platform/sdl_window.h"
#include "studio/studio_ui.h"
#include "studio/validation_world.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::filesystem::path projectDir = platz::platform::GetRepoRootDir() / "projects" / "PLATZ-Validation";
    bool headless = false;
    int verifyFrames = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--verify-frames" && i + 1 < argc) {
            verifyFrames = std::max(1, std::atoi(argv[++i]));
        }
    }

    std::cout << "[PLATZ] Starting PLATZ Studio (One Place. Zero Limits.)\n";
    std::cout << "[PLATZ] Canonical Project Path: " << projectDir.string() << "\n";

    // 1. Open or populate the permanent living validation world (projects/PLATZ-Validation)
    platz::core::ProjectState project;
    std::string err;
    if (!platz::studio::ValidationWorldManager::EnsureValidationWorldPopulated(project, projectDir, &err)) {
        std::cerr << "[PLATZ ERROR] Failed initializing project at " << projectDir.string() << ": " << err << "\n";
        return 1;
    }
    std::cout << "[PLATZ] Loaded project '" << project.GetProjectName()
              << "' at revision r" << project.GetHeadRevision()
              << " (" << project.GetObjects().size() << " canonical objects, "
              << project.GetAssets().size() << " content-addressed assets).\n";

    // 2. Initialize SDL3 Platform Window & Vulkan 1.4 Renderer
    platz::platform::WindowConfig winCfg{};
    winCfg.title = "PLATZ Studio — " + project.GetProjectName() + " [Vulkan 1.4]";
    winCfg.width = 1600;
    winCfg.height = 920;
    winCfg.headless = headless;

    platz::platform::SdlWindow window;
    if (!window.Initialize(winCfg, &err)) {
        // If running in a headless terminal session where Win32 display creation fails, fall back cleanly to headless Vulkan
        std::cout << "[PLATZ] Window creation note (" << err << "); continuing in headless Vulkan 1.4 mode.\n";
        winCfg.headless = true;
        window.Initialize(winCfg, &err);
    }

    platz::engine::vulkan::VulkanRenderer renderer;
    if (!renderer.Initialize(&window, 1280, 720, true, &err)) {
        std::cerr << "[PLATZ ERROR] Vulkan 1.4 Renderer initialization failed: " << err << "\n";
        return 1;
    }

    const auto& caps = renderer.GetContext().GetCapabilities();
    std::cout << "[PLATZ] Active GPU: " << caps.deviceName
              << " (Vulkan " << renderer.GetContext().GetFormattedApiVersion()
              << ", DynamicRendering=" << (caps.supportsDynamicRendering ? "YES" : "NO")
              << ", Sync2=" << (caps.supportsSynchronization2 ? "YES" : "NO")
              << ", Validation=" << (renderer.GetContext().IsValidationActive() ? "ACTIVE" : "OFF") << ")\n";

    // 3. Initialize Runtime, CommandRegistry, StagedAiSession, SpecialistModel, and StudioUiController
    platz::engine::runtime::RuntimeSession runtime;
    platz::ai::CommandRegistry commandRegistry;
    platz::ai::StagedAiSession stagedAiSession(commandRegistry);
    platz::ai::NativeMaterialSpecialistModel specialistModel;

    platz::studio::StudioUiController studioUi(
        project,
        renderer,
        runtime,
        commandRegistry,
        stagedAiSession,
        specialistModel);

    auto lastTick = std::chrono::high_resolution_clock::now();
    bool running = true;
    int framesRendered = 0;

    while (running) {
        if (!window.IsHeadless()) {
            SDL_Event ev{};
            while (SDL_PollEvent(&ev)) {
                if (ImGui::GetCurrentContext()) {
                    ImGui_ImplSDL3_ProcessEvent(&ev);
                }
                if (ev.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                }
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;

        if (runtime.IsPlaying()) {
            runtime.StepSimulation(dt);
        }

        bool renderOk = false;
        if (window.IsHeadless()) {
            renderOk = renderer.RenderSceneOffscreen(
                project,
                studioUi.GetActiveSceneObjects(),
                studioUi.GetCamera(),
                studioUi.GetSelectedObjectId(),
                &err);
        } else {
            renderOk = renderer.RenderInteractiveFrame(
                project,
                studioUi.GetActiveSceneObjects(),
                studioUi.GetCamera(),
                studioUi.GetSelectedObjectId(),
                [&]() { studioUi.BuildImGuiStudioFrame(); },
                &err);
        }

        if (!renderOk) {
            std::cerr << "[PLATZ ERROR] Frame render error: " << err << "\n";
            return 1;
        }

        ++framesRendered;
        if (verifyFrames > 0 && framesRendered >= verifyFrames) {
            running = false;
        }
    }

    // If --verify-frames was requested, run the automated validation route, capture GPU readback frame, and write receipt
    if (verifyFrames > 0) {
        auto routeReceipt = platz::studio::ValidationWorldManager::RunAutomatedVerificationRoute(project);
        platz::creative::ImageData2D frameCapture;
        bool readbackOk = renderer.ReadbackViewportColorRgba(&frameCapture, &err);
        std::filesystem::path capturePath = platz::platform::GetRepoRootDir() / "out" / "verification_viewport_capture.png";
        if (readbackOk) {
            platz::creative::TextureGenerator::WriteImageToPng(frameCapture, capturePath);
        }

        const auto& perf = renderer.GetPerfMetrics();
        auto budget = renderer.GetContext().GetMemoryBudgetSnapshot();

        nlohmann::json receipt = {
            {"product", "PLATZ"},
            {"executablePath", (platz::platform::GetExecutableDir() / "PLATZ.exe").string()},
            {"projectDir", project.GetProjectDir().string()},
            {"headRevision", project.GetHeadRevision()},
            {"gpuDeviceName", caps.deviceName},
            {"vulkanApiVersion", renderer.GetContext().GetFormattedApiVersion()},
            {"validationLayerActive", renderer.GetContext().IsValidationActive()},
            {"validationErrorCount", renderer.GetContext().GetValidationErrorCount()},
            {"framesRendered", framesRendered},
            {"viewportResolution", {renderer.GetViewportWidth(), renderer.GetViewportHeight()}},
            {"lastCpuFrameTimeMs", perf.cpuFrameTimeMs},
            {"lastGpuScenePassTimeMs", perf.gpuScenePassTimeMs},
            {"drawCallCount", perf.drawCallCount},
            {"triangleCount", perf.triangleCount},
            {"vmaActiveAllocatedBytes", budget.activeAllocatedBytes},
            {"vmaPeakAllocatedBytes", budget.peakAllocatedBytes},
            {"validationWorldRoute", routeReceipt.ToJson()},
            {"specialistModelReceipt", specialistModel.GetActiveReceipt().ToJson()},
            {"viewportCapturePng", capturePath.string()},
            {"timestampUtc", platz::platform::UtcIso8601Now()}
        };

        std::filesystem::path receiptPath = platz::platform::GetRepoRootDir() / "out" / "verification_receipt.json";
        {
            std::ofstream ofs(receiptPath, std::ios::binary | std::ios::trunc);
            ofs << receipt.dump(2);
        }

        std::cout << "[PLATZ VERIFY] Rendered " << framesRendered << " Vulkan 1.4 frames on " << caps.deviceName
                  << " (CPU=" << perf.cpuFrameTimeMs << " ms, GPU=" << perf.gpuScenePassTimeMs
                  << " ms, ValidationErrors=" << renderer.GetContext().GetValidationErrorCount()
                  << ", StationsPassed=" << routeReceipt.stationsPassed << "/" << routeReceipt.stationsVisited << ")\n";
        std::cout << "[PLATZ VERIFY] Receipt written to: " << receiptPath.string() << "\n";

        if (!routeReceipt.allStationsPassed || renderer.GetContext().GetValidationErrorCount() > 0) {
            renderer.Shutdown();
            window.Shutdown();
            return 2;
        }
    }

    renderer.Shutdown();
    window.Shutdown();
    return 0;
}
