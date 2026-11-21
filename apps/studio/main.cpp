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
    float initialUiScale = 1.0f;
    std::string workspaceArg;
    std::string windowCapturePathArg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--verify-frames" && i + 1 < argc) {
            verifyFrames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--ui-scale" && i + 1 < argc) {
            initialUiScale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--workspace" && i + 1 < argc) {
            workspaceArg = argv[++i];
        } else if (arg == "--capture-window" && i + 1 < argc) {
            windowCapturePathArg = argv[++i];
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

    // 2. Initialize SDL3 Platform Window & Vulkan 1.4 Renderer at realistic 1920x1080 desktop resolution
    platz::platform::WindowConfig winCfg{};
    winCfg.title = "PLATZ Studio — " + project.GetProjectName() + " [Vulkan 1.4]";
    winCfg.width = 1920;
    winCfg.height = 1040;
    winCfg.headless = headless;

    platz::platform::SdlWindow window;
    if (!window.Initialize(winCfg, &err)) {
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

    if (initialUiScale != 1.0f) {
        studioUi.SetUiScale(initialUiScale);
    }
    if (workspaceArg == "materials") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::MaterialsAndShaders);
    } else if (workspaceArg == "pixel") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::PixelArtAndSprites);
    } else if (workspaceArg == "ai") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::PlatzAiWorkbench);
    } else if (workspaceArg == "model") {
        studioUi.SetWorkspaceMode(platz::studio::WorkspaceMode::ModelAndAssets);
    }

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

    if (verifyFrames > 0) {
        auto routeReceipt = platz::studio::ValidationWorldManager::RunAutomatedVerificationRoute(project);

        // 1. Capture raw 3D offscreen viewport image
        platz::creative::ImageData2D frameCapture;
        bool readbackOk = renderer.ReadbackViewportColorRgba(&frameCapture, &err);
        std::filesystem::path capturePath = platz::platform::GetRepoRootDir() / "out" / "verification_viewport_capture.png";
        if (readbackOk) {
            platz::creative::TextureGenerator::WriteImageToPng(frameCapture, capturePath);
        }

        // 2. Capture full composed Studio application window swapchain image
        platz::creative::ImageData2D windowCapture;
        bool winReadbackOk = renderer.ReadbackSwapchainWindowRgba(&windowCapture, &err);
        std::filesystem::path winCapturePath = windowCapturePathArg.empty()
            ? (platz::platform::GetRepoRootDir() / "out" / "verification_studio_window.png")
            : std::filesystem::path(windowCapturePathArg);
        if (winReadbackOk) {
            platz::creative::TextureGenerator::WriteImageToPng(windowCapture, winCapturePath);
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
            {"windowResolution", {windowCapture.width, windowCapture.height}},
            {"viewportResolution", {renderer.GetViewportWidth(), renderer.GetViewportHeight()}},
            {"uiDpiScaleFactor", studioUi.GetUiScale()},
            {"lastCpuFrameTimeMs", perf.cpuFrameTimeMs},
            {"lastGpuScenePassTimeMs", perf.gpuScenePassTimeMs},
            {"drawCallCount", perf.drawCallCount},
            {"triangleCount", perf.triangleCount},
            {"vmaActiveAllocatedBytes", budget.activeAllocatedBytes},
            {"vmaPeakAllocatedBytes", budget.peakAllocatedBytes},
            {"validationWorldRoute", routeReceipt.ToJson()},
            {"materialSpecialistReceipt", specialistModel.GetActiveReceipt().ToJson()},
            {"gameplaySpecialistReceipt", studioUi.GetGameplaySpecialist().GetActiveReceipt().ToJson()},
            {"studioWindowCapturePng", winCapturePath.string()},
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
        std::cout << "[PLATZ VERIFY] Studio Window Screenshot: " << winCapturePath.string() << "\n";
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
